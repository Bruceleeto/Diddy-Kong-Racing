#include <stddef.h>

#include "audiomgr.h"
#include "asset_loading.h"
#include "common.h"
#include "math_util.h"
#include "memory.h"
#include "objects.h"
#include "PR/abi.h"
#include "rcp_dkr.h"
#include "stacks.h"
#include "video.h"

/****  type define's for structures unique to audiomgr ****/
typedef union {

    struct {
        short type;
    } gen;

    struct {
        short type;
        struct AudioInfo_s *info;
    } done;

    OSScMsg app;

} AudioMsg;

// TODO: Rare likely modified this struct as evidenced by __amHandleFrameMsg
// using *data when it should have been using msg.
typedef struct AudioInfo_s {
    short *data;        /* Output data pointer */
    short frameSamples; /* # of samples synthesized in this frame */
    OSScTask task;      /* scheduler structure */
    AudioMsg msg;       /* completion message */
} AudioInfo;

#ifdef TARGET_PC
// amCreateAudioMgr only alHeapAllocs 120 bytes per AudioInfo, but the struct is
// 152. Retail has the same overrun: `msg` lands at offset 120, one byte past the
// allocation, and is never read or written by anything (see the TODO above — the
// completion message is passed as &info->data instead). So the invariant that
// actually matters is that everything LIVE — data, frameSamples, task — fits
// inside the 120 bytes, i.e. msg begins exactly at the end of them. If the host
// ever lays these out differently, the three output buffers start scribbling on
// each other.
_Static_assert(offsetof(struct AudioInfo_s, msg) == 120, "AudioInfo's live fields no longer fit the 120-byte alloc");
#endif

typedef struct {
    Acmd *ACMDList[NUM_ACMD_LISTS];
    AudioInfo *audioInfo[NUM_OUTPUT_BUFFERS];
    OSThread thread;
    OSMesgQueue audioFrameMsgQ;
    OSMesg audioFrameMsgBuf[MAX_MESGS];
    OSMesgQueue audioReplyMsgQ;
    OSMesg audioReplyMsgBuf[MAX_MESGS];
    ALGlobals g;
} AMAudioMgr;

typedef struct {
    ALLink node;
    u32 startAddr;
    u32 lastFrame;
    char *ptr;
} AMDMABuffer;

typedef struct {
    u8 initialized;
    AMDMABuffer *firstUsed;
    AMDMABuffer *firstFree;
} AMDMAState;

/**** audio manager globals ****/

OSSched *gAudioSched;
ALHeap *gAudioHeap; // Set but not used

AMAudioMgr __am;
static u64 audioStack[STACKSIZE(STACK_AUD)];

AMDMAState dmaState;
AMDMABuffer dmaBuffs[NUM_DMA_BUFFERS];
u32 audFrameCt = 0;
u32 nextDMA = 0;
u32 curAcmdList = 0;
u32 minFrameSize;
u32 frameSize;
u32 maxFrameSize;
s32 gAudioCmdLen; // Set but not used

/**** Anti Piracy - Sets random audio frequency ****/
s16 gAntiPiracyCRCStart;
s8 gAntiPiracyAudioFreq = FALSE;
s32 gRaceCheckFinishChecksum = Func80019808Checksum;
s32 gRaceCheckFinishFuncLength = 0xFD0;

/** Queues and storage for use with audio DMA's ****/
OSIoMesg audDMAIOMesgBuf[NUM_DMA_MESSAGES];
OSMesgQueue audDMAMessageQ;
OSMesg audDMAMessageBuf[NUM_DMA_MESSAGES];

/**** Not really sure why these are here. They are set, but never used. ****/
static s16 *gLastAudioPtr = 0;
static s32 gLastAudioFrameSamples = 0;

#ifdef TARGET_PC
// linux/reimpl.c — "ROM" reads served from the loaded asset image, and the shared
// byteswap helper (asset sub-tables come back as raw big-endian u32s).
extern void pc_dmacopy(u32 romOffset, u32 ramAddress, s32 numBytes);
extern void pc_swap32_buf(void *buf, u32 numBytes);

// linux/audio_hle.c — the aspMain microcode's replacement: a C interpreter of the
// RSP audio command list the synthesizer just built. It SAVEBUFFs the mixed PCM
// into `out` itself (see alSavePull), so there is nothing to copy afterwards.
extern void pc_audio_hle_run(void *cmdList, s32 cmdLen, void *out, s32 frameSamples);

static void pc_audio_submit(Acmd *cmdList, s32 cmdLen, s16 *out, s32 frameSamples) {
    if (out == NULL || frameSamples <= 0) {
        return;
    }
    bzero(out, frameSamples * 4); // stereo s16; the list may not cover every sample
    pc_audio_hle_run(cmdList, cmdLen, out, frameSamples);
}
#endif

/**** private routines ****/
static void __amMain(UNUSED void *arg);
static s32 __amDMA(s32 addr, s32 len, void *state);
static ALDMAproc __amDmaNew(AMDMAState **state);
static u32 __amHandleFrameMsg(AudioInfo *info, AudioInfo *lastInfo);
static void __amHandleDoneMsg(AudioInfo *info);
static void __clearAudioDMA(void);

/**** Debug strings ****/
const char D_800E49F0[] =
    "audio manager: RCP audio interface bug caused DMA from bad address - move audiomgr.c in the makelist!\n";

/******************************************************************************
 * Audio Manager API
 *****************************************************************************/
void amCreateAudioMgr(ALSynConfig *c, OSPri pri, OSSched *audSched) {
    s32 i;
    f32 fsize;
    uintptr_t *asset;
    u32 *assetAudioTable;
    s32 *asset8;
    s32 assetSize;
    s32 checksum;
    u8 *crc_region_start;
    u8 *crc_region;

    gAudioSched = audSched;
    gAudioHeap = c->heap;

    c->dmaproc = &__amDmaNew;
    c->outputRate = osAiSetFrequency(OUTPUT_RATE);

    /*
     * Calculate the frame sample parameters from the
     * video field rate and the output rate
     */
    fsize = (f32) c->outputRate * 2 / (f32) gVideoRefreshRate;
    frameSize = (s32) fsize;
    if (frameSize < fsize) {
        frameSize++;
    }
    if (frameSize & 0xf) {
        frameSize = (frameSize & ~0xf) + 0x10;
    }
    minFrameSize = frameSize - 16;
    maxFrameSize = frameSize + EXTRA_SAMPLES + 16;

    if (c->fxType[0] == AL_FX_CUSTOM) {
        assetAudioTable = asset_table_load(ASSET_AUDIO_TABLE);
#ifdef TARGET_PC
        // This is a SECOND, fresh copy of the audio table — audio_init swapped its
        // own copy, not this one. Every asset_table_load returns raw big-endian
        // u32s and has to be swapped by whoever loads it.
        pc_swap32_buf(assetAudioTable, asset_table_size(ASSET_AUDIO_TABLE));
#endif
        assetSize = assetAudioTable[ASSET_AUDIO_9] - assetAudioTable[ASSET_AUDIO_8];
        asset8 = mempool_alloc_safe(assetSize, COLOUR_TAG_CYAN);
        asset_load(ASSET_AUDIO, (u32) asset8, assetAudioTable[ASSET_AUDIO_8], assetSize);
#ifdef TARGET_PC
        // The AL_FX_CUSTOM reverb params: a flat array of big-endian s32s
        // (section_count, length, then five per delay section — see alFxNew in
        // drvrnew.c). Unswapped, section_count/length read as garbage and alFxNew
        // asks the pool for a 0-byte delay line.
        pc_swap32_buf(asset8, assetSize);
#endif
        c->params = asset8;
#ifndef TARGET_PC
        // OUT-OF-BOUNDS WRITE, and always has been: `params` is ALSynConfig's last
        // field, so c[1].maxVVoices is the word just past the struct — i.e. the
        // caller's stack frame (audio_init's `synth_config`). The intent is to NULL
        // "params for bus 1", because alFxNew reads it as (&c->params)[bus].
        //
        // It is dead: that read only happens when fxType[bus] == AL_FX_CUSTOM, and
        // DKR sets fxType[1] = AL_FX_BIGROOM. On N64 the stray word landed in frame
        // padding and nobody noticed; on PC -fstack-protector turns it into an
        // instant abort (ASan: stack-buffer-overflow, audio_init's frame).
        //
        // If a future config ever sets fxType[1] = AL_FX_CUSTOM, this needs a real
        // params[2] field, not a wider stack frame.
        c[1].maxVVoices = 0;
#endif
        alInit(&__am.g, c);
        //!@bug: Forgot to free assetAudioTable
        mempool_free(asset8);
    } else {
        alInit(&__am.g, c);
    }

    dmaBuffs[0].node.prev = 0;
    dmaBuffs[0].node.next = 0;

    for (i = 0; i < NUM_DMA_BUFFERS - 1; i++) {
        alLink((ALLink *) &(dmaBuffs[i + 1]), (ALLink *) &(dmaBuffs[i]));
        dmaBuffs[i].ptr = alHeapAlloc(c->heap, 1, DMA_BUFFER_LENGTH);
    }
    /* last buffer already linked, but still needs buffer */
    dmaBuffs[i].ptr = alHeapAlloc(c->heap, 1, DMA_BUFFER_LENGTH);
#ifdef ANTI_TAMPER
    // Antipiracy measure
    gAntiPiracyCRCStart = DMA_BUFFER_LENGTH;
    checksum = (s32) &race_check_finish - gAntiPiracyCRCStart;
    crc_region_start = (u8 *) checksum;
    crc_region = &crc_region_start[gAntiPiracyCRCStart];
    gAntiPiracyAudioFreq = FALSE;
    i = 0;
    for (checksum = 0; i < gRaceCheckFinishFuncLength; i++) {
        checksum += crc_region[i];
        gAntiPiracyCRCStart++;
    }
    if (checksum != gRaceCheckFinishChecksum) {
        gAntiPiracyAudioFreq = TRUE;
    }
#endif

    for (i = 0; i < NUM_ACMD_LISTS; i++) {
        __am.ACMDList[i] = (Acmd *) alHeapAlloc(c->heap, 1, AUDBUF_SIZE);
    }

#ifdef TARGET_PC
    // N64 pins the three output buffers just below the top of RAM. RAM_END is
    // meaningless against a host address space (gMainMemoryPool is a static array),
    // so take them from the pool like everything else. Same size, same layout —
    // the `asset += maxFrameSize` stride below is unchanged.
    asset = mempool_alloc_safe((maxFrameSize * 12), COLOUR_TAG_CYAN);
#else
    asset = mempool_alloc_fixed((maxFrameSize * 12), (u8 *) ((RAM_END - 0x200) - (maxFrameSize * 12)), COLOUR_TAG_CYAN);
#endif

    /**** initialize the done messages ****/
    for (i = 0; i < NUM_ACMD_LISTS + 1; i++) {
        __am.ACMDList[i + NUM_ACMD_LISTS] = (Acmd *) alHeapAlloc(c->heap, 1, 120);
        __am.ACMDList[i + NUM_ACMD_LISTS]->words.w0 = (uintptr_t) asset;
        asset += maxFrameSize;
    }

    osCreateMesgQueue(&__am.audioReplyMsgQ, __am.audioReplyMsgBuf, MAX_MESGS);
    osCreateMesgQueue(&__am.audioFrameMsgQ, __am.audioFrameMsgBuf, MAX_MESGS);
    osCreateMesgQueue(&audDMAMessageQ, audDMAMessageBuf, NUM_DMA_MESSAGES);
#ifndef TARGET_PC
    // No thread system on PC (__osDispatchThread is a stub): a thread created here
    // would never be scheduled. am_audio_frame_pc() below is the replacement — the
    // main loop calls it once per frame, doing what __amMain's retrace case does.
    osCreateThread(&__am.thread, 4, __amMain, 0, (void *) (audioStack + STACKSIZE(STACK_AUD)), pri);
#endif
}

/**
 * Official Name: amGo
 */
void audioStartThread(void) {
#ifndef TARGET_PC
    osStartThread(&__am.thread);
#endif
}

/**
 * Official Name: amGoStop
 */
void audioStopThread(void) {
#ifndef TARGET_PC
    osStopThread(&__am.thread);
#endif
}

#ifdef TARGET_PC
/**
 * PC/Dreamcast: the audio thread's per-retrace tick, called from the host frame
 * loop (linux/audio.c) instead of from a scheduler message. This is __amMain's
 * OS_SC_RETRACE_MSG case with the message-queue round trip removed: there is no
 * RSP to send the task to and no thread to block in, so __amHandleFrameMsg runs
 * the command list synchronously (see the TARGET_PC path there) and the "done"
 * reply is implicit.
 *
 * lastInfo lags one frame on purpose — the N64 hands the AI the buffer the RSP
 * filled *last* frame while the synth fills the next one. Keeping that lag keeps
 * the frameSamples feedback identical to the console's.
 */
void am_audio_frame_pc(void) {
    static AudioInfo *lastInfo = NULL;
    AudioInfo *info;

    if (gAudioSched == NULL) {
        return; // audio_init hasn't run yet
    }

    info = (AudioInfo *) __am.ACMDList[(audFrameCt % 3) + 2];
    __amHandleFrameMsg(info, lastInfo);
    __amHandleDoneMsg(info);
    lastInfo = info;
}
#endif

/******************************************************************************
 *
 * Audio Manager implementation. This thread wakes up at every retrace,
 * and builds an audio task, which it returns to the scheduler, who then
 * is responsible for its finally execution on the RSP. Once the task has
 * finished execution, the scheduler sends back a message saying the task
 * is complete. The audio is triple buffered because the switching to a new
 * audio buffer does not occur exactly at the gfx swapbuffer time.  With
 * 3 buffers you ensure that the program does not destroy data before it is
 * played.
 *
 *****************************************************************************/
/**
 * Main function for handling ingame audio. Loops continuously as long as the scheduler feeds it updates.
 */
static void __amMain(UNUSED void *arg) {
    s32 done = 0;
    AudioMsg *msg = NULL;
    AudioInfo *lastInfo = 0;

    osScAddClient(gAudioSched, (OSScClient *) &audioStack, &__am.audioFrameMsgQ, OS_MESG_BLOCK);

    while (!done) {
        (void) osRecvMesg(&__am.audioFrameMsgQ, (OSMesg *) &msg, OS_MESG_BLOCK);
        switch (msg->gen.type) {
            case OS_SC_RETRACE_MSG:
                // TODO: Check type of ACMDList?
                __amHandleFrameMsg((AudioInfo *) __am.ACMDList[(((u32) audFrameCt % 3)) + 2], lastInfo);
                /* wait for done message */
                osRecvMesg(&__am.audioReplyMsgQ, (OSMesg *) &lastInfo, OS_MESG_BLOCK);
                __amHandleDoneMsg(lastInfo);
                break;
            case OS_SC_PRE_NMI_MSG:
                /* what should we really do here? quit? ramp down volume? */
                break;
            case QUIT_MSG:
                done = 1;
                break;
            default:
                break;
        }
    }

    alClose(&__am.g);
}

/******************************************************************************
 *
 * __amHandleFrameMsg. First, clear the past audio dma's, then calculate
 * the number of samples you will need for this frame. This value varies
 * due to the fact that audio is synchronised off of the video interupt
 * which can have a small amount of jitter in it. Varying the number of
 * samples slightly will allow you to stay in synch with the video. This
 * is an advantageous thing to do, since if you are in synch with the
 * video, you will have fewer graphics yields. After you've calculated
 * the number of frames needed, call alAudioFrame, which will call all
 * of the synthesizer's players (sequence player and sound player) to
 * generate the audio task list. If you get a valid task list back, put
 * it in a task structure and send a message to the scheduler to let it
 * know that the next frame of audio is ready for processing.
 *
 *****************************************************************************/
static u32 __amHandleFrameMsg(AudioInfo *info, AudioInfo *lastInfo) {
    s16 *audioPtr;
    Acmd *cmdp;
    int samplesLeft = 0;
    OSScTask *t;
    u32 ret;

    __clearAudioDMA(); /* call once a frame, before doing alAudioFrame */

    audioPtr = (s16 *) osVirtualToPhysical(info->data);

    if (lastInfo) {
        s16 *outputDataPointer;
        s32 frameSamples;

        gLastAudioPtr = outputDataPointer = lastInfo->data;
        gLastAudioFrameSamples = frameSamples = lastInfo->frameSamples << 2;
        osAiSetNextBuffer(outputDataPointer, frameSamples);
#ifdef ANTI_TAMPER
        // Antipiracy measure
        if (gAntiPiracyAudioFreq) {
            osAiSetFrequency(rand_range(0, 10000) + OUTPUT_RATE);
        }
#endif
    }

    /* calculate how many samples needed for this frame to keep the DAC full */
    /* this will vary slightly frame to frame, must recalculate every frame */
    samplesLeft = osAiGetLength() >> 2; /* divide by four, to convert bytes */
                                        /* to stereo 16 bit samples */
    info->frameSamples = (16 + (frameSize - samplesLeft + EXTRA_SAMPLES)) & ~0xf;
    if ((u32) info->frameSamples < minFrameSize) {
        info->frameSamples = minFrameSize;
    }

    cmdp = alAudioFrame(__am.ACMDList[curAcmdList], &gAudioCmdLen, audioPtr, info->frameSamples);

#ifdef TARGET_PC
    // No RSP and no scheduler: run the command list the synthesizer just built,
    // right here, instead of packaging it into an OSScTask. pc_audio_submit is the
    // aspMain microcode's replacement (M3); until it exists it just clears the
    // output buffer, which is what makes M2 "silent but correctly paced".
    pc_audio_submit(__am.ACMDList[curAcmdList], (s32) (cmdp - __am.ACMDList[curAcmdList]), audioPtr,
                    info->frameSamples);
    curAcmdList ^= 1;
    return 0;
#else

    t = &info->task;

    t->next = 0;                    /* paranoia */
    t->msgQ = &__am.audioReplyMsgQ; /* reply to when finished */
    // TODO: This should be &info->msg, but the struct is likely modified.
    t->msg = (OSMesg) &info->data; /* reply with this message */
    t->flags = OS_SC_NEEDS_RSP;
    t->unk58 = -1;
    t->unk60 = 0xFF;
    t->unk5C = 0;
    t->unk64 = 0;

    t->list.t.data_ptr = (u64 *) __am.ACMDList[curAcmdList];
    t->list.t.data_size = (cmdp - __am.ACMDList[curAcmdList]) * sizeof(Acmd);
    t->list.t.type = M_AUDTASK;
    t->list.t.ucode_boot = (u64 *) rspbootTextStart;
    t->list.t.ucode_boot_size = (s32) ((s32) rspbootTextEnd - (s32) rspbootTextStart);
    t->list.t.flags = OS_TASK_DP_WAIT;
    t->list.t.ucode = (u64 *) aspMainTextStart;
    t->list.t.ucode_data = (u64 *) aspMainDataStart;
    t->list.t.ucode_data_size = SP_UCODE_DATA_SIZE;
    t->list.t.yield_data_ptr = NULL;
    t->list.t.yield_data_size = 0;
    t->unk6C = 1;

    ret = osSendMesg(osScGetCmdQ(gAudioSched), (OSMesg) t, OS_MESG_NOBLOCK);

    curAcmdList ^= 1; /* swap which acmd list you use each frame */

    return ret;
#endif
}

/******************************************************************************
 *
 * __amHandleDoneMsg. Really just debugging info in this frame. Checks
 * to make sure we completed before we were out of samples.
 *
 *****************************************************************************/
static void __amHandleDoneMsg(UNUSED AudioInfo *info) {
    s32 samplesLeft;
    static int firstTime = 1;

    samplesLeft = osAiGetLength() >> 2;
    if (samplesLeft == 0 && !firstTime) {
        stubbed_printf("audio: ai out of samples\n");
        firstTime = 0;
    }
}

/******************************************************************************
 *
 * __amDMA This routine handles the dma'ing of samples from rom to ram.
 * First it checks the current buffers to see if the samples needed are
 * already in place. Because buffers are linked sequentially by the
 * addresses where the samples are on rom, it doesn't need to check all
 * of them, only up to the address that it needs. If it finds one, it
 * returns the address of that buffer. If it doesn't find the samples
 * that it needs, it will initiate a DMA of the samples that it needs.
 * In either case, it updates the lastFrame variable, to indicate that
 * this buffer was last used in this frame. This is important for the
 * __clearAudioDMA routine.
 *
 *****************************************************************************/
static s32 __amDMA(s32 addr, s32 len, UNUSED void *state) {
    void *foundBuffer;
    s32 delta, addrEnd, buffEnd;
    AMDMABuffer *dmaPtr, *lastDmaPtr;
    UNUSED s32 pad;

    lastDmaPtr = NULL;
    delta = addr & 1;
    dmaPtr = dmaState.firstUsed;
    addrEnd = addr + len;

    /* first check to see if a currently existing buffer contains the
       sample that you need.  */

    while (dmaPtr) {
        buffEnd = dmaPtr->startAddr + DMA_BUFFER_LENGTH;
        if (dmaPtr->startAddr > (u32) addr) { /* since buffers are ordered */
            break;                            /* abort if past possible */
        } else if (addrEnd <= buffEnd) {      /* yes, found a buffer with samples */
            dmaPtr->lastFrame = audFrameCt;   /* mark it used */
            foundBuffer = dmaPtr->ptr + addr - dmaPtr->startAddr;
            return (int) osVirtualToPhysical(foundBuffer);
        }
        lastDmaPtr = dmaPtr;
        dmaPtr = (AMDMABuffer *) dmaPtr->node.next;
    }

    /* get here, and you didn't find a buffer, so dma a new one */

    /* get a buffer from the free list */
    dmaPtr = dmaState.firstFree;

    if (!dmaPtr && !lastDmaPtr) {
        lastDmaPtr = dmaState.firstUsed;
    }

    /*
     * if you get here and dmaPtr is null, send back a bogus
     * pointer, it's better than nothing
     */
    if (!dmaPtr) {
        stubbed_printf("OH DEAR - No audio DMA buffers left\n");
        return (int) osVirtualToPhysical(lastDmaPtr->ptr) + delta;
    }

    dmaState.firstFree = (AMDMABuffer *) dmaPtr->node.next;
    alUnlink((ALLink *) dmaPtr);

    /* add it to the used list */
    if (lastDmaPtr) { /* if you have other dmabuffers used, add this one */
                      /* to the list, after the last one checked above */
        alLink((ALLink *) dmaPtr, (ALLink *) lastDmaPtr);
    } else if (dmaState.firstUsed) { /* if this buffer is before any others */
                                     /* jam at begining of list */
        lastDmaPtr = dmaState.firstUsed;
        dmaState.firstUsed = dmaPtr;
        dmaPtr->node.next = (ALLink *) lastDmaPtr;
        dmaPtr->node.prev = 0;
        lastDmaPtr->node.prev = (ALLink *) dmaPtr;
    } else { /* no buffers in list, this is the first one */
        dmaState.firstUsed = dmaPtr;
        dmaPtr->node.next = 0;
        dmaPtr->node.prev = 0;
    }

    foundBuffer = dmaPtr->ptr;
    addr -= delta;
    dmaPtr->startAddr = addr;
    dmaPtr->lastFrame = audFrameCt; /* mark it */

#ifdef TARGET_PC
    // No PI and no cart: "ROM" is assets.bin, resident in host memory. Copy
    // synchronously, then post the completion message __clearAudioDMA expects to
    // receive once per DMA issued — skipping it would make every frame report
    // "Dma not done". (RAW16 sample data was already swapped in the asset image at
    // bank-load time; ADPCM is a byte stream. See docs/audio.md.)
    pc_dmacopy((u32) addr, (u32) foundBuffer, DMA_BUFFER_LENGTH);
    osSendMesg(&audDMAMessageQ, (OSMesg) &audDMAIOMesgBuf[nextDMA++], OS_MESG_NOBLOCK);
#else
    osPiStartDma(&audDMAIOMesgBuf[nextDMA++], OS_MESG_PRI_HIGH, OS_READ, addr, foundBuffer, DMA_BUFFER_LENGTH,
                 &audDMAMessageQ);
#endif

    return (int) osVirtualToPhysical(foundBuffer) + delta;
}

/******************************************************************************
 *
 * __amDmaNew.  Initialize the dma buffers and return the address of the
 * procedure that will be used to dma the samples from rom to ram. This
 * routine will be called once for each physical voice that is created.
 * In this case, because we know where all the buffers are, and since
 * they are not attached to a specific voice, we will only really do any
 * initialization the first time. After that we just return the address
 * to the dma routine.
 *
 *****************************************************************************/
static ALDMAproc __amDmaNew(AMDMAState **state) {

    if (!dmaState.initialized) { /* only do this once */
        dmaState.firstUsed = 0;
        dmaState.firstFree = &dmaBuffs[0];
        dmaState.initialized = 1;
    }

    *state = &dmaState; /* state is never used in this case */

    return __amDMA;
}

/******************************************************************************
 *
 * __clearAudioDMA.  Routine to move dma buffers back to the unused list.
 * First clear out your dma messageQ. Then check each buffer to see when
 * it was last used. If that was more than FRAME_LAG frames ago, move it
 * back to the unused list.
 *
 *****************************************************************************/
static void __clearAudioDMA(void) {
    u32 i;
    OSIoMesg *iomsg = 0;
    AMDMABuffer *dmaPtr, *nextPtr;

    /*
     * Don't block here. If dma's aren't complete, you've had an audio
     * overrun. (Bad news, but go for it anyway, and try and recover.)
     */
    for (i = 0; i < nextDMA; i++) {
        if (osRecvMesg(&audDMAMessageQ, (OSMesg *) &iomsg, OS_MESG_NOBLOCK) == -1) {
            stubbed_printf("Dma not done\n");
        }
        // if (logging)
        //     osLogEvent(log, 17, 2, iomsg->devAddr, iomsg->size);
    }

    dmaPtr = dmaState.firstUsed;
    while (dmaPtr) {
        nextPtr = (AMDMABuffer *) dmaPtr->node.next;

        /* remove old dma's from list */
        /* Can change FRAME_LAG value.  Should be at least one.  */
        /* Larger values mean more buffers needed, but fewer DMA's */
        if (dmaPtr->lastFrame + FRAME_LAG < audFrameCt) {
            if (dmaState.firstUsed == dmaPtr) {
                dmaState.firstUsed = (AMDMABuffer *) dmaPtr->node.next;
            }
            alUnlink((ALLink *) dmaPtr);
            if (dmaState.firstFree) {
                alLink((ALLink *) dmaPtr, (ALLink *) dmaState.firstFree);
            } else {
                dmaState.firstFree = dmaPtr;
                dmaPtr->node.next = 0;
                dmaPtr->node.prev = 0;
            }
        }
        dmaPtr = nextPtr;
    }

    nextDMA = 0; /* reset */
    audFrameCt++;
}
