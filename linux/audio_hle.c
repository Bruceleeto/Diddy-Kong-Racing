// HLE of the aspMain RSP audio microcode — the replacement for the RSP itself.
//
// The libultra synthesizer (libultra/src/audio/) does not mix anything: it builds
// a list of commands for the RSP, and the RSP does the work. There is no RSP here,
// so this file executes that command list in C: a 4KB DMEM, 15 opcodes, ABI1.
//
// PORTABLE C ON PURPOSE. It is the hot loop of the audio system and it is also the
// part the Dreamcast needs verbatim — the only platform-specific audio file is
// linux/audio.c (the SDL ring). Do not put x86-isms or SDL calls in here.
//
// ---------------------------------------------------------------------------
// Endianness: the whole design rests on one rule.
// ---------------------------------------------------------------------------
// Everything in DMEM and in the RDRAM audio buffers is HOST-NATIVE s16, because we
// own both ends of it: this file writes those buffers and this file reads them back
// (reverb delay lines), and SDL consumes the final output. The only data crossing
// in from the big-endian world is sample data out of the asset image, and that was
// already handled at bank-load time (docs/audio.md, M1):
//
//   - ADPCM sample frames  : a BYTE stream. Never swapped, anywhere. LOADBUFF
//                            memcpy's them raw and the decoder reads nibbles.
//   - RAW16 sample data    : BE s16, swapped once in the asset image itself
//                            (pc_asset_swap16_region), so LOADBUFF's memcpy is
//                            already host-native.
//   - ADPCM codebooks      : swapped as part of ALADPCMBook in bnkf.c.
//
// So there is not a single byteswap in this file. If you find yourself wanting to
// add one, the bug is upstream.

#include <string.h>

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed long long s64;

// ---------------------------------------------------------------------------
// ABI1 opcodes (include/PR/abi.h)
// ---------------------------------------------------------------------------
#define A_SPNOOP 0
#define A_ADPCM 1
#define A_CLEARBUFF 2
#define A_ENVMIXER 3
#define A_LOADBUFF 4
#define A_RESAMPLE 5
#define A_SAVEBUFF 6
#define A_SEGMENT 7
#define A_SETBUFF 8
#define A_SETVOL 9
#define A_DMEMMOVE 10
#define A_LOADADPCM 11
#define A_MIXER 12
#define A_INTERLEAVE 13
#define A_POLEF 14
#define A_SETLOOP 15

// Flags
#define A_INIT 0x01
#define A_CONTINUE 0x00
#define A_LOOP 0x02
#define A_LEFT 0x02
#define A_RIGHT 0x00
#define A_VOL 0x04
#define A_RATE 0x00
#define A_AUX 0x08

#define DMEM_SIZE 4096

// ---------------------------------------------------------------------------
// RSP state
// ---------------------------------------------------------------------------
static u8 sDmem[DMEM_SIZE];

static u16 sIn, sOut, sCount;            // SETBUFF, no flags
static u16 sDryRight, sWetLeft, sWetRight; // SETBUFF | A_AUX
static s16 sVol[2];                      // [0]=left, [1]=right
static s16 sTarget[2];
static s32 sRate[2];
static s16 sDry, sWet;
static s16 sTable[512]; // ADPCM codebook, also POLEF coefficients
static u32 sLoopAddr;

// Identity "physical" addresses: osVirtualToPhysical is identity on the host, so a
// DMA address in the command list is just a host pointer.
static void *rdram(u32 addr) {
    return (void *) (unsigned long) addr;
}

static s16 clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (s16) v;
}

// DMEM is byte-addressed but every audio buffer in it is s16-aligned.
static s16 *dmem16(u32 addr) {
    return (s16 *) &sDmem[addr & (DMEM_SIZE - 2)];
}

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

static void op_setbuff(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;

    if (flags & A_AUX) {
        // The aux form reuses the three fields as three DMEM addresses — see
        // alMainBusPull: aSetBuffer(A_AUX, AL_MAIN_R_OUT, AL_AUX_L_OUT, AL_AUX_R_OUT).
        // The last one is NOT a count here.
        sDryRight = w0 & 0xFFFF;
        sWetLeft = (w1 >> 16) & 0xFFFF;
        sWetRight = w1 & 0xFFFF;
    } else {
        sIn = w0 & 0xFFFF;
        sOut = (w1 >> 16) & 0xFFFF;
        sCount = w1 & 0xFFFF;
    }
}

static void op_setvol(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 v = (s16) (w0 & 0xFFFF);
    u16 t = (w1 >> 16) & 0xFFFF;
    u16 r = w1 & 0xFFFF;

    if (flags & A_AUX) {
        sDry = v;
        sWet = (s16) r;
    } else if (flags & A_VOL) {
        sVol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        // A_RATE: v is the envelope target, (t:r) the 16.16 per-sample step.
        s32 idx = (flags & A_LEFT) ? 0 : 1;
        sTarget[idx] = v;
        sRate[idx] = (s32) (((u32) t << 16) | r);
    }
}

static void op_clearbuff(u32 w0, u32 w1) {
    u32 addr = w0 & 0xFFFFFF;
    u32 count = w1 & 0xFFFF;

    if (addr + count > DMEM_SIZE) {
        count = DMEM_SIZE - addr;
    }
    memset(&sDmem[addr], 0, count);
}

static void op_dmemmove(u32 w0, u32 w1) {
    u32 src = w0 & 0xFFFFFF;
    u32 dst = (w1 >> 16) & 0xFFFF;
    u32 count = w1 & 0xFFFF;

    if (src + count > DMEM_SIZE || dst + count > DMEM_SIZE) {
        return;
    }
    memmove(&sDmem[dst], &sDmem[src], count); // may overlap
}

static void op_loadbuff(u32 w1) {
    u32 count = sCount;

    if (sIn + count > DMEM_SIZE) {
        count = DMEM_SIZE - sIn;
    }
    // Raw bytes. ADPCM frames stay a byte stream; RAW16 was swapped in the asset
    // image already. See the header comment.
    memcpy(&sDmem[sIn], rdram(w1), count);
}

static void op_savebuff(u32 w1) {
    u32 count = sCount;

    if (sOut + count > DMEM_SIZE) {
        count = DMEM_SIZE - sOut;
    }
    memcpy(rdram(w1), &sDmem[sOut], count);
}

static void op_loadadpcm(u32 w0, u32 w1) {
    // aLoadADPCM(pkt, c, d): count in w0 (24 bits), DRAM address in w1. Count is in
    // BYTES; the table is s16 entries. Doubles as the POLEF coefficient load
    // (alFilterNew does aLoadADPCM(32, fccoef)).
    u32 count = w0 & 0xFFFFFF;
    u32 entries = count >> 1;
    const s16 *src = rdram(w1);
    u32 i;

    if (entries > 512) {
        entries = 512;
    }
    // Host-native: the codebook was swapped with the rest of ALADPCMBook, and the
    // POLEF coefficients are computed at runtime.
    for (i = 0; i < entries; i++) {
        sTable[i] = src[i];
    }
}

// A_MIXER: out += in * gain, saturating. gain is s16 (0x7fff == ~1.0).
static void op_mix(u32 w0, u32 w1) {
    s16 gain = (s16) (w0 & 0xFFFF);
    u16 dmemi = (w1 >> 16) & 0xFFFF;
    u16 dmemo = w1 & 0xFFFF;
    s16 *src = dmem16(dmemi);
    s16 *dst = dmem16(dmemo);
    s32 n = sCount >> 1; // bytes -> samples
    s32 k;

    for (k = 0; k < n; k++) {
        dst[k] = clamp16(dst[k] + (((s32) src[k] * (s32) gain) >> 15));
    }
}

// A_INTERLEAVE: weave two mono buffers into stereo at `out`. Writes 2*count bytes
// (alSavePull sets count to the MONO byte count, then re-sets it to count<<2 before
// the SAVEBUFF that follows).
static void op_interleave(u32 w1) {
    u16 leftAddr = (w1 >> 16) & 0xFFFF;
    u16 rightAddr = w1 & 0xFFFF;
    const s16 *l = dmem16(leftAddr);
    const s16 *r = dmem16(rightAddr);
    s16 *dst = dmem16(sOut);
    s32 n = sCount >> 1; // samples per channel
    s32 k;

    for (k = 0; k < n; k++) {
        dst[2 * k + 0] = l[k];
        dst[2 * k + 1] = r[k];
    }
}

// The RSP audio microcode's resampling filter: 64 phases of a 4-tap FIR. This is
// the real thing, taken from the ucode (via sm64-port's mixer.c, which decodes the
// same aspMain ABI we do). It replaces the linear interpolation this used to do —
// linear is a cheap approximation that costs high-frequency aliasing, audible as
// warble/grit on pitched notes.
static const s16 sResampleTable[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};



// ---------------------------------------------------------------------------
// A_RESAMPLE — pitch shift, 4-tap polyphase (ucode-accurate).
//
// The phase accumulator is 16.16: `pitch` from the command is a u16 that steps by
// pitch*2 per output sample. The low 16 bits select one of 64 filter phases
// (accu >> 10); the integer part advances the input window.
//
// State (16 s16 in RDRAM):
//   [0..3] the 4 input samples at the window position — the filter needs 4 taps of
//          history, and they must survive across command lists or every frame
//          boundary is a discontinuity (i.e. a click)
//   [4]    the fractional phase
// ---------------------------------------------------------------------------
static void op_resample(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    u32 pitch = (w0 & 0xFFFF) << 1; // 16.16 step per output sample
    s16 *state = rdram(w1);
    const s16 *src = dmem16(sIn);
    s16 *dst = dmem16(sOut);
    s32 n = sCount >> 1; // output samples wanted
    u32 accu;
    s16 hist[4];
    s32 pos = 0; // window start in the virtual stream (hist, then src)
    s32 k;

    if (flags & A_INIT) {
        memset(hist, 0, sizeof(hist));
        accu = 0;
    } else {
        memcpy(hist, &state[0], sizeof(hist));
        accu = (u16) state[4];
    }

    // The virtual input stream is the 4 history samples followed by the DMEM input
    // buffer, so the filter window is continuous across command lists.
#define RESAMPLE_TAP(i) (((i) < 4) ? hist[i] : src[(i) - 4])

    for (k = 0; k < n; k++) {
        const s16 *tbl = sResampleTable[accu >> 10]; // 64 phases
        s32 sample = ((RESAMPLE_TAP(pos + 0) * tbl[0] + 0x4000) >> 15) +
                     ((RESAMPLE_TAP(pos + 1) * tbl[1] + 0x4000) >> 15) +
                     ((RESAMPLE_TAP(pos + 2) * tbl[2] + 0x4000) >> 15) +
                     ((RESAMPLE_TAP(pos + 3) * tbl[3] + 0x4000) >> 15);

        dst[k] = clamp16(sample);

        accu += pitch;
        pos += accu >> 16;
        accu &= 0xFFFF;
    }

    // Carry the window and the phase into the next command list.
    for (k = 0; k < 4; k++) {
        state[k] = RESAMPLE_TAP(pos + k);
    }
    state[4] = (s16) (u16) accu;

#undef RESAMPLE_TAP
}

// ---------------------------------------------------------------------------
// A_ADPCM — Nintendo 4-bit ADPCM, 16 samples per 9-byte frame.
//
// Frame: 1 header byte (scale in the high nibble, predictor index in the low), then
// 8 bytes of 16 packed 4-bit nibbles.
//
// State is ADPCM_STATE (16 s16) = the last 16 samples decoded, which is also the
// layout of ALADPCMloop.state in the bank (the 16 samples preceding a loop point) —
// so A_LOOP can load it straight from there. That shared layout is why this is a
// fixed format and not ours to choose.
// ---------------------------------------------------------------------------
static void op_adpcm(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 *state = rdram(w1);
    const u8 *src = &sDmem[sIn];
    s16 *dst = dmem16(sOut);
    s32 outSamples = sCount >> 1;
    s16 hist[16];
    s32 produced = 0;

    if (flags & A_INIT) {
        memset(hist, 0, sizeof(hist));
    } else if (flags & A_LOOP) {
        memcpy(hist, rdram(sLoopAddr), sizeof(hist)); // loop-point context, from the bank
    } else {
        memcpy(hist, state, sizeof(hist));
    }

    // The two most recently decoded samples carry the prediction across frames (and
    // across command lists, via `state`).
    {
        s32 l1 = hist[15];
        s32 l2 = hist[14];

        while (produced < outSamples) {
            u8 header = *src++;
            s32 scale = header >> 4;
            s32 predIdx = header & 0xF;
            // order 2 => 16 coefficients per predictor: book1[8] then book2[8].
            const s16 *book1 = &sTable[predIdx * 16];
            const s16 *book2 = &sTable[predIdx * 16 + 8];
            s32 residual[16];
            s16 frame[16];
            s32 i, j, k, half;

            // 16 packed 4-bit residuals, sign-extended and scaled by 2^scale.
            for (i = 0; i < 16; i++) {
                u8 byte = src[i >> 1];
                s32 nibble = (i & 1) ? (byte & 0xF) : (byte >> 4);

                if (nibble > 7) {
                    nibble -= 16;
                }
                residual[i] = nibble << scale;
            }
            src += 8;

            // VADPCM proper: each 8-sample half is an order-2 prediction from the
            // previous two OUTPUT samples, convolved with the codebook, plus the
            // contribution of the residuals already decoded within this half. It is
            // not a 2-tap IIR — the codebook rows are 8 taps deep.
            for (half = 0; half < 2; half++) {
                for (j = 0; j < 8; j++) {
                    s32 acc = (s32) book1[j] * l2 + (s32) book2[j] * l1;

                    for (k = 0; k < j; k++) {
                        acc += (s32) book2[j - k - 1] * residual[half * 8 + k];
                    }
                    acc = (acc >> 11) + residual[half * 8 + j];
                    frame[half * 8 + j] = clamp16(acc);
                }
                l2 = frame[half * 8 + 6];
                l1 = frame[half * 8 + 7];
            }

            for (i = 0; i < 16 && produced < outSamples; i++, produced++) {
                dst[produced] = frame[i];
            }
            memcpy(hist, frame, sizeof(hist));
        }
    }

    memcpy(state, hist, sizeof(hist)); // carry into the next command list
}

// ---------------------------------------------------------------------------
// A_ENVMIXER — the volume/pan/envelope stage, and the only opcode with four
// outputs: dry L/R (the main bus) and wet L/R (the reverb bus).
//
// State (40 s16, ours): [0..1] the two 16.16 volume accumulators.
// ---------------------------------------------------------------------------
static void op_envmixer(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 *state = rdram(w1);
    const s16 *src = dmem16(sIn);
    s16 *dryL = dmem16(sOut);
    s16 *dryR = dmem16(sDryRight);
    s16 *wetL = dmem16(sWetLeft);
    s16 *wetR = dmem16(sWetRight);
    s32 n = sCount >> 1;
    s32 volAccu[2];
    s32 k;

    if (flags & A_INIT) {
        volAccu[0] = (s32) sVol[0] << 16;
        volAccu[1] = (s32) sVol[1] << 16;
    } else {
        volAccu[0] = ((s32) (u16) state[0] << 16) | (u16) state[1];
        volAccu[1] = ((s32) (u16) state[2] << 16) | (u16) state[3];
    }

    for (k = 0; k < n; k++) {
        s32 s = src[k];
        s32 vl = volAccu[0] >> 16;
        s32 vr = volAccu[1] >> 16;
        s32 l = (s * vl) >> 15;
        s32 r = (s * vr) >> 15;

        dryL[k] = clamp16(dryL[k] + ((l * sDry) >> 15));
        dryR[k] = clamp16(dryR[k] + ((r * sDry) >> 15));
        if (flags & A_AUX) {
            wetL[k] = clamp16(wetL[k] + ((l * sWet) >> 15));
            wetR[k] = clamp16(wetR[k] + ((r * sWet) >> 15));
        }

        // Ramp toward the target — ONCE EVERY 8 SAMPLES, not every sample.
        //
        // The rate is a signed 16.16 step per 8-sample group, not per sample. See
        // _getRate() in env.c: it computes (tgt - vol) / count and then multiplies
        // by 8, and _getVol() reads it back as `ivol += r * samples / 8`. Stepping
        // it every sample ramps every envelope 8x too fast: attacks snap, and
        // decays/releases hit their target (usually zero) in an eighth of the time,
        // so notes cut out early and the volume steps hard enough to crackle.
        if ((k & 7) == 7) {
            s32 i;

            for (i = 0; i < 2; i++) {
                if (sRate[i] == 0) {
                    continue; // steady volume: hold it, don't snap to the target
                }
                volAccu[i] += sRate[i];
                if (sRate[i] > 0 ? (volAccu[i] >> 16) > sTarget[i] : (volAccu[i] >> 16) < sTarget[i]) {
                    volAccu[i] = (s32) sTarget[i] << 16;
                }
            }
        }
    }

    state[0] = (s16) (u16) (volAccu[0] >> 16);
    state[1] = (s16) (u16) (volAccu[0] & 0xFFFF);
    state[2] = (s16) (u16) (volAccu[1] >> 16);
    state[3] = (s16) (u16) (volAccu[1] & 0xFFFF);
}

// A_POLEF — the reverb bus's one-pole lowpass. Coefficients come from the table
// loaded by the LOADADPCM immediately before it (alFilterNew: aLoadADPCM(32, ...)).
static void op_polef(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 gain = (s16) (w0 & 0xFFFF);
    s16 *state = rdram(w1);
    s16 *src = dmem16(sIn);
    s16 *dst = dmem16(sOut);
    s32 n = sCount >> 1;
    s32 y1, y2;
    s32 k;

    if (flags & A_INIT) {
        y1 = 0;
        y2 = 0;
    } else {
        y1 = state[0];
        y2 = state[1];
    }

    for (k = 0; k < n; k++) {
        s32 acc = ((s32) src[k] * (s32) gain) >> 14;

        acc += ((s32) sTable[0] * y1 + (s32) sTable[8] * y2) >> 14;
        y2 = y1;
        y1 = clamp16(acc);
        dst[k] = (s16) y1;
    }

    state[0] = (s16) y1;
    state[1] = (s16) y2;
}

// ---------------------------------------------------------------------------
// The interpreter
// ---------------------------------------------------------------------------
void pc_audio_hle_run(void *cmdList, s32 cmdLen, void *outBuf, s32 frameSamples) {
    const u32 *cmd = cmdList;
    s32 i;

    (void) outBuf;      // the list SAVEBUFFs into it itself
    (void) frameSamples;

    for (i = 0; i < cmdLen; i++) {
        u32 w0 = cmd[2 * i + 0];
        u32 w1 = cmd[2 * i + 1];
        u32 op = w0 >> 24;

        switch (op) {
            case A_SPNOOP:
                break;
            case A_SEGMENT:
                break; // segment 0, base 0 — addresses are already absolute
            case A_SETBUFF:
                op_setbuff(w0, w1);
                break;
            case A_SETVOL:
                op_setvol(w0, w1);
                break;
            case A_CLEARBUFF:
                op_clearbuff(w0, w1);
                break;
            case A_DMEMMOVE:
                op_dmemmove(w0, w1);
                break;
            case A_LOADBUFF:
                op_loadbuff(w1);
                break;
            case A_SAVEBUFF:
                op_savebuff(w1);
                break;
            case A_LOADADPCM:
                op_loadadpcm(w0, w1);
                break;
            case A_SETLOOP:
                sLoopAddr = w1;
                break;
            case A_ADPCM:
                op_adpcm(w0, w1);
                break;
            case A_RESAMPLE:
                op_resample(w0, w1);
                break;
            case A_ENVMIXER:
                op_envmixer(w0, w1);
                break;
            case A_MIXER:
                op_mix(w0, w1);
                break;
            case A_INTERLEAVE:
                op_interleave(w1);
                break;
            case A_POLEF:
                op_polef(w0, w1);
                break;
            default:
                break;
        }
    }
}
