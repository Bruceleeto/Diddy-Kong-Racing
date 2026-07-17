/*====================================================================
 * bnkf.c
 *
 * Copyright 1993, Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Silicon Graphics,
 * Inc.; the contents of this file may not be disclosed to third
 * parties, copied or duplicated in any form, in whole or in part,
 * without the prior written permission of Silicon Graphics, Inc.
 *
 * RESTRICTED RIGHTS LEGEND:
 * Use, duplication or disclosure by the Government is subject to
 * restrictions as set forth in subdivision (c)(1)(ii) of the Rights
 * in Technical Data and Computer Software clause at DFARS
 * 252.227-7013, and/or in similar or successor clauses in the FAR,
 * DOD or NASA FAR Supplement. Unpublished - rights reserved under the
 * Copyright Laws of the United States.
 *====================================================================*/

#include <libaudio.h>
#include <os_internal.h>
#include <ultraerror.h>

/*
 * ### when the file format settles down a little, I'll remove these
 * ### for efficiency.
 */
static  void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table);
static  void _bnkfPatchInst(ALInstrument *i, s32 offset, s32 table);
static  void _bnkfPatchSound(ALSound *s, s32 offset, s32 table);
static  void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table);

#ifdef TARGET_PC
/*
 * PC/Dreamcast: the bank file is a big-endian on-disk image whose "pointers" are
 * really BE u32 file offsets. Every multi-byte field has to be swapped BEFORE the
 * patch code below reads it (it branches on instCount/soundCount/type, so a
 * swap-after would walk garbage). The swap piggybacks on this traversal rather
 * than duplicating it in a second walk that could drift out of sync.
 *
 * Visited-guards: ALBank/ALInstrument/ALSound/ALWaveTable each carry a `flags`
 * byte the patcher already uses to avoid double-patching shared nodes, so those
 * are safe. ALEnvelope, ALADPCMBook and ALADPCMloop have NO such field and CAN be
 * shared between sounds — swapping one twice would silently un-swap it. Hence the
 * pointer registry below. This is the single subtlest thing in the file.
 */
#include <stddef.h>

extern void isv_printf(const char *fmt, ...);

/* Sample data lives in the asset image and is DMA'd on demand; RAW16 waves are
 * BE s16 and must be swapped there, at the source. (linux/reimpl.c) */
extern void pc_asset_swap16_region(u32 romOffset, u32 numBytes);

#define BNKF_MAX_SHARED 2048
static void *sBnkfSwapped[BNKF_MAX_SHARED];
static s32   sBnkfSwappedCount;

/* Returns 1 the first time a node is seen, 0 on every later visit. */
static s32 _bnkfFirstVisit(void *p)
{
    s32 i;

    if (p == NULL)
        return 0;
    for (i = 0; i < sBnkfSwappedCount; i++) {
        if (sBnkfSwapped[i] == p)
            return 0;
    }
    if (sBnkfSwappedCount == BNKF_MAX_SHARED) {
        /* Registry full: past this point a shared node could be swapped twice,
         * which silently un-swaps it. Fail loudly rather than corrupt quietly;
         * bumping BNKF_MAX_SHARED is the fix if this ever fires. */
        isv_printf("*** bnkf: shared-node registry overflow (>%d) — bank data WILL be wrong ***\n", BNKF_MAX_SHARED);
        return 0;
    }
    sBnkfSwapped[sBnkfSwappedCount++] = p;
    return 1;
}

static u16 _bnkfSwap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static u32 _bnkfSwap32(u32 v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

#define BSWAP16(f) ((f) = _bnkfSwap16((u16)(f)))
#define BSWAP32(f) ((f) = _bnkfSwap32((u32)(f)))
/* Pointer fields hold BE u32 file offsets until the patch code adds the base. */
#define BSWAPPTR(f) ((f) = (void *) _bnkfSwap32((u32)(f)))

/* Layout proofs: the host must lay these out exactly as the N64 did, or the
 * offsets we are swapping are not the fields we think they are. */
_Static_assert(sizeof(ALWaveTable) == 20, "ALWaveTable layout drift");
_Static_assert(offsetof(ALWaveTable, len) == 4, "ALWaveTable.len moved");
_Static_assert(offsetof(ALWaveTable, type) == 8, "ALWaveTable.type moved");
_Static_assert(offsetof(ALWaveTable, waveInfo) == 12, "ALWaveTable.waveInfo moved");
_Static_assert(sizeof(ALSound) == 16, "ALSound layout drift");
_Static_assert(offsetof(ALSound, wavetable) == 8, "ALSound.wavetable moved");
_Static_assert(offsetof(ALBank, sampleRate) == 4, "ALBank.sampleRate moved");
_Static_assert(offsetof(ALBank, instArray) == 12, "ALBank.instArray moved");
_Static_assert(offsetof(ALInstrument, soundArray) == 16, "ALInstrument.soundArray moved");
/* 3 x s32 + 2 x u8, padded to a 4-byte boundary. */
_Static_assert(sizeof(ALEnvelope) == 16, "ALEnvelope layout drift");
_Static_assert(sizeof(ALADPCMloop) == 44, "ALADPCMloop layout drift");
_Static_assert(sizeof(ALSeqData) == 8, "ALSeqData layout drift");

static void _bnkfSwapEnvelope(ALEnvelope *e)
{
    if (!_bnkfFirstVisit(e))
        return;
    BSWAP32(e->attackTime);
    BSWAP32(e->decayTime);
    BSWAP32(e->releaseTime);
    /* attackVolume/decayVolume are u8 */
}

static void _bnkfSwapBook(ALADPCMBook *b)
{
    s32 i, entries;

    if (!_bnkfFirstVisit(b))
        return;
    /* order/npredictors describe the length of the book that follows them, so
     * they must be swapped FIRST and only then used to size the loop. */
    BSWAP32(b->order);
    BSWAP32(b->npredictors);
    entries = b->npredictors * b->order * 8;
    for (i = 0; i < entries; i++) {
        BSWAP16(b->book[i]);
    }
}

static void _bnkfSwapADPCMLoop(ALADPCMloop *l)
{
    s32 i;

    if (!_bnkfFirstVisit(l))
        return;
    BSWAP32(l->start);
    BSWAP32(l->end);
    BSWAP32(l->count);
    for (i = 0; i < 16; i++) { /* ADPCM_STATE: 16 BE s16 predictor values */
        BSWAP16(l->state[i]);
    }
}

static void _bnkfSwapRawLoop(ALRawLoop *l)
{
    if (!_bnkfFirstVisit(l))
        return;
    BSWAP32(l->start);
    BSWAP32(l->end);
    BSWAP32(l->count);
}
#endif /* TARGET_PC */

void alSeqFileNew(ALSeqFile *file, u8 *base)
{
    s32 offset = (s32) base;
    s32 i;

#ifdef TARGET_PC
    BSWAP16(file->revision);
    BSWAP16(file->seqCount);
    for (i = 0; i < file->seqCount; i++) {
        BSWAPPTR(file->seqArray[i].offset);
        BSWAP32(file->seqArray[i].len);
    }
#endif

    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->seqCount; i++) {
        file->seqArray[i].offset = (u8 *)((u8 *)file->seqArray[i].offset + offset);
    }
}

void alBnkfNew(ALBankFile *file, u8 *table)
{
    s32 offset = (s32) file;
    s32 woffset = (s32) table;

    s32 i;

#ifdef TARGET_PC
    /* Fresh image, fresh registry (each bank file is a separate allocation). */
    sBnkfSwappedCount = 0;
    BSWAP16(file->revision);
    BSWAP16(file->bankCount);
    for (i = 0; i < file->bankCount; i++) {
        BSWAPPTR(file->bankArray[i]);
    }
#endif

    /*
     * check the file format revision in debug libraries
     */
    ALFailIf(file->revision != AL_BANK_VERSION, ERR_ALBNKFNEW);

    /*
     * patch the file so that offsets are pointers
     */
    for (i = 0; i < file->bankCount; i++) {
        file->bankArray[i] = (ALBank *)((u8 *)file->bankArray[i] + offset);
        if(file->bankArray[i])
            _bnkfPatchBank(file->bankArray[i], offset, woffset);
    }
}

void _bnkfPatchBank(ALBank *bank, s32 offset, s32 table)
{
    s32 i;

    if (bank->flags)
        return;

    bank->flags = 1;

#ifdef TARGET_PC
    BSWAP16(bank->instCount);
    BSWAP32(bank->sampleRate);
    BSWAPPTR(bank->percussion);
    for (i = 0; i < bank->instCount; i++) {
        BSWAPPTR(bank->instArray[i]);
    }
#endif

    if (bank->percussion) {
        bank->percussion = (ALInstrument *)((u8 *)bank->percussion + offset);
        _bnkfPatchInst(bank->percussion, offset, table);
    }
    
    for (i = 0; i < bank->instCount; i++) {
        bank->instArray[i] = (ALInstrument *)((u8 *)bank->instArray[i] +
                                              offset);
        if(bank->instArray[i])
            _bnkfPatchInst(bank->instArray[i], offset, table);
    }
}

void _bnkfPatchInst(ALInstrument *inst, s32 offset, s32 table)
{
    s32 i;

    if (inst->flags)
        return;

    inst->flags = 1;

#ifdef TARGET_PC
    BSWAP16(inst->bendRange);
    BSWAP16(inst->soundCount);
    for (i = 0; i < inst->soundCount; i++) {
        BSWAPPTR(inst->soundArray[i]);
    }
    /* volume, pan, priority, and the tremolo/vibrato fields are all u8 */
#endif

    for (i = 0; i < inst->soundCount; i++) {
        inst->soundArray[i] = (ALSound *)((u8 *)inst->soundArray[i] +
                                          offset);
        _bnkfPatchSound(inst->soundArray[i], offset, table);

    }
}

void _bnkfPatchSound(ALSound *s, s32 offset, s32 table)
{
    if (s->flags)
        return;

    s->flags = 1;

#ifdef TARGET_PC
    BSWAPPTR(s->envelope);
    BSWAPPTR(s->keyMap);
    BSWAPPTR(s->wavetable);
    /* samplePan/sampleVolume are u8 */
#endif

    s->envelope  = (ALEnvelope *)((u8 *)s->envelope + offset);
    s->keyMap    = (ALKeyMap *)((u8 *)s->keyMap + offset);

#ifdef TARGET_PC
    /* Envelopes can be shared between sounds; ALEnvelope has no visited flag of
     * its own, so _bnkfSwapEnvelope guards against the double swap.
     * ALKeyMap is all u8 - nothing to swap. */
    _bnkfSwapEnvelope(s->envelope);
#endif

    s->wavetable = (ALWaveTable *)((u8 *)s->wavetable + offset);
    _bnkfPatchWaveTable(s->wavetable, offset, table);
}

void _bnkfPatchWaveTable(ALWaveTable *w, s32 offset, s32 table)
{
    if (w->flags)
        return;

    w->flags = 1;

#ifdef TARGET_PC
    /* base is a BE file offset here; it becomes a ROM address once `table` is
     * added below. type/flags are u8. */
    BSWAPPTR(w->base);
    BSWAP32(w->len);
    if (w->type == AL_ADPCM_WAVE) {
        BSWAPPTR(w->waveInfo.adpcmWave.book);
        BSWAPPTR(w->waveInfo.adpcmWave.loop);
    } else if (w->type == AL_RAW16_WAVE) {
        BSWAPPTR(w->waveInfo.rawWave.loop);
    }
#endif

    w->base += table;

    /* sct 2/14/96 - patch wavetable loop info based on type. */
    if (w->type == AL_ADPCM_WAVE)
    {
	w->waveInfo.adpcmWave.book  = (ALADPCMBook *)((u8 *)w->waveInfo.adpcmWave.book + offset);
	if (w->waveInfo.adpcmWave.loop)
	    w->waveInfo.adpcmWave.loop = (ALADPCMloop *)((u8 *)w->waveInfo.adpcmWave.loop + offset);

#ifdef TARGET_PC
	_bnkfSwapBook(w->waveInfo.adpcmWave.book);
	_bnkfSwapADPCMLoop(w->waveInfo.adpcmWave.loop);
	/* The ADPCM sample frames themselves are a BYTE stream. Do not swap them.
	 * Swapping them is silent corruption that sounds like white noise at the
	 * right rhythm - see docs/audio.md. */
#endif
    }
    else if (w->type == AL_RAW16_WAVE)
    {
	if (w->waveInfo.rawWave.loop)
	    w->waveInfo.rawWave.loop = (ALRawLoop *)((u8 *)w->waveInfo.rawWave.loop + offset);

#ifdef TARGET_PC
	_bnkfSwapRawLoop(w->waveInfo.rawWave.loop);
	/* RAW16 sample data IS big-endian s16. It is streamed straight out of the
	 * asset image by __amDMA, which has no idea what type of wave it is
	 * serving - so the swap has to happen at the source, here, where the type
	 * and the ROM base are both known. w->flags gates this to once per wave. */
	pc_asset_swap16_region((u32) w->base, (u32) w->len);
#endif
    }
}
