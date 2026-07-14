// Host audio backend for the PC build.
//
// This is the ONLY platform-specific audio file. Everything above it — the
// libultra ALSynth sequencer/synthesizer, the audio manager, the (M3) command-list
// interpreter — is portable C. The Dreamcast port replaces this file with a KOS
// snd_stream backend and changes nothing else. Keep it that way: no game logic
// here, no mixing here.
//
// Two jobs:
//
//   1. The AI (Audio Interface) shims. On N64 these poke MMIO registers; the three
//      the game actually uses become a ring buffer here. osAiGetLength() is NOT a
//      throwaway stub — __amHandleFrameMsg recomputes how many samples to
//      synthesize each frame from it, so it has to honestly report how much audio
//      is still queued or the frame-size feedback loop goes unstable.
//
//   2. The frame pump. There is no audio thread and no scheduler on PC (see
//      linux/reimpl.c), so the audio manager is ticked once per video frame from
//      the main loop instead — same shape as the gfx-task and thread-30 shims in
//      rcp_dkr.c and thread30_bgload.c.

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

typedef signed short s16;
typedef signed int s32;
typedef unsigned char u8;
typedef unsigned int u32;

// The audio manager's per-frame tick (src/audiomgr.c, TARGET_PC path).
extern void am_audio_frame_pc(void);

// src/audiomgr.c: samples of audio the manager wants to produce per tick. Derived
// from outputRate * 2 / refreshRate = 735 at NTSC — i.e. 1/30s of audio, because
// the audio thread ticks once per game frame (two video fields), not per field.
extern unsigned int frameSize;

#define PC_AUDIO_RATE 22050
#define PC_AUDIO_CHANNELS 2
#define PC_AUDIO_BYTES_PER_SAMPLE 4 // stereo s16

// Ring capacity. Generous: the game submits one video frame of audio at a time
// (~735 stereo samples, ~2.9KB) and we want slack for scheduling jitter without
// letting latency grow unbounded.
#define PC_RING_BYTES (64 * 1024)

static SDL_AudioDeviceID sAudioDev;
static u8 sRing[PC_RING_BYTES];
static u32 sRingRead;
static u32 sRingWrite;
static u32 sRingUsed; // bytes queued and not yet played
static s32 sAudioReady;
static u32 sUnderruns;

// SDL pulls from the ring on its own thread; every touch of the ring state is
// under SDL_LockAudioDevice (the producer) or inside this callback (the consumer).
static void pc_audio_callback(void *unused, u8 *stream, int len) {
    u32 n = (u32) len;

    if (n > sRingUsed) {
        // Underrun: hand SDL what we have and pad the rest with silence. Do not
        // stall — the game's frame loop is what refills us, and blocking here
        // would deadlock against it.
        u32 have = sRingUsed;
        u32 i;

        for (i = 0; i < have; i++) {
            stream[i] = sRing[sRingRead];
            sRingRead = (sRingRead + 1) % PC_RING_BYTES;
        }
        memset(stream + have, 0, n - have);
        sRingUsed = 0;
        sUnderruns++;
        return;
    }

    {
        u32 i;

        for (i = 0; i < n; i++) {
            stream[i] = sRing[sRingRead];
            sRingRead = (sRingRead + 1) % PC_RING_BYTES;
        }
        sRingUsed -= n;
    }
}

// ---------------------------------------------------------------------------
// AI shims (replacing libultra/src/io/ai.c, aigetlen.c, aisetfreq.c,
// aisetnextbuf.c — all dropped from the PC build, they only poke MMIO)
// ---------------------------------------------------------------------------

// Called once from amCreateAudioMgr with OUTPUT_RATE. Returns the rate actually
// achieved; the manager derives its whole frame-size schedule from the value we
// return here, so it must be the truth.
s32 osAiSetFrequency(u32 frequency) {
    SDL_AudioSpec want, got;

    if (sAudioReady) {
        return (s32) PC_AUDIO_RATE;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "AUDIO: SDL_InitSubSystem failed: %s — running silent\n", SDL_GetError());
        return (s32) frequency;
    }

    memset(&want, 0, sizeof(want));
    want.freq = (int) frequency;
    want.format = AUDIO_S16SYS;
    want.channels = PC_AUDIO_CHANNELS;
    want.samples = 512;
    want.callback = pc_audio_callback;

    sAudioDev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (sAudioDev == 0) {
        fprintf(stderr, "AUDIO: SDL_OpenAudioDevice failed: %s — running silent\n", SDL_GetError());
        return (s32) frequency;
    }

    sAudioReady = 1;
    SDL_PauseAudioDevice(sAudioDev, 0);
    printf("AUDIO: %d Hz, %d ch, %d-sample buffer\n", got.freq, got.channels, got.samples);
    return got.freq;
}

// The game hands us the PCM the mixer produced for the *previous* frame.
void osAiSetNextBuffer(void *buf, u32 size) {
    const u8 *src = buf;
    u32 i;

    if (!sAudioReady || buf == NULL || size == 0) {
        return;
    }

    SDL_LockAudioDevice(sAudioDev);
    if (sRingUsed + size > PC_RING_BYTES) {
        // Overrun: the game is producing faster than the DAC drains. Dropping the
        // newest buffer keeps latency bounded. If this fires steadily, the
        // frame-size feedback via osAiGetLength() is wrong.
        SDL_UnlockAudioDevice(sAudioDev);
        return;
    }
    for (i = 0; i < size; i++) {
        sRing[sRingWrite] = src[i];
        sRingWrite = (sRingWrite + 1) % PC_RING_BYTES;
    }
    sRingUsed += size;
    SDL_UnlockAudioDevice(sAudioDev);
}

// True ring occupancy — what the pump below paces against.
static u32 pc_audio_queued(void) {
    u32 used;

    if (!sAudioReady) {
        return 0;
    }
    SDL_LockAudioDevice(sAudioDev);
    used = sRingUsed;
    SDL_UnlockAudioDevice(sAudioDev);
    return used;
}

// Bytes still to play. This is the feedback signal __amHandleFrameMsg uses to size
// the next chunk:
//
//     frameSamples = (16 + (frameSize - osAiGetLength()/4 + 96)) & ~0xf
//
// On N64 the AI holds at most two buffers — one playing, one pending — and
// osAiGetLength() returns what is left of the *playing* one, so it is never more
// than a single frame. Our ring is much deeper than that, and reporting the whole
// backlog makes (frameSize - samplesLeft) go NEGATIVE. The game's clamp below it
// tests `(u32) info->frameSamples < minFrameSize`, so a negative value casts to a
// huge unsigned, sails straight through the clamp, and osAiSetNextBuffer gets a
// ~4GB length. (Observed: frameSamples = -208, then a 16MB overread of the memory
// pool.)
//
// So: saturate at one frame, exactly like the hardware. The formula then always
// lands in [112, 848] and the clamp does its job. Total latency is governed by the
// pump's target below, not by this value.
u32 osAiGetLength(void) {
    u32 used = pc_audio_queued();
    u32 oneFrame;

    if (frameSize == 0) {
        return used; // before amCreateAudioMgr has run
    }
    oneFrame = frameSize * PC_AUDIO_BYTES_PER_SAMPLE;
    return (used > oneFrame) ? oneFrame : used;
}

// ---------------------------------------------------------------------------
// Frame pump
// ---------------------------------------------------------------------------

// Called once per video frame from linux/main.c, standing in for the audio
// thread's OS_SC_RETRACE_MSG wakeup — but NOT one tick per call.
//
// The manager produces 1/30s of audio per tick, so it needs ticking at 30Hz. The
// host frame rate is whatever the host frame rate is (60, 144, hitching, vsync
// off), and pumping once per frame at 60Hz produces audio at 2x the rate the DAC
// drains it — the backlog runs away and never recovers.
//
// So pace against the ring instead of against the frame: top it up to a target
// depth and stop. This is self-correcting and completely independent of host fps,
// which is also what the Dreamcast port will want when this moves onto a
// vblank-driven audio thread.
#define PC_AUDIO_TARGET_FRAMES 3 // ~100ms of buffered audio
#define PC_AUDIO_MAX_TICKS 4     // don't spin forever if something goes wrong

void pc_audio_frame(void) {
    u32 target;
    s32 ticks = 0;

    if (!sAudioReady || frameSize == 0) {
        return;
    }
    target = frameSize * PC_AUDIO_BYTES_PER_SAMPLE * PC_AUDIO_TARGET_FRAMES;

    while (pc_audio_queued() < target && ticks < PC_AUDIO_MAX_TICKS) {
        am_audio_frame_pc();
        ticks++;
    }
}

// Diagnostics — M2's whole deliverable is "the pacing is stable", and this is how
// we see it.
void pc_audio_report(void) {
    static u32 lastUnderruns;

    if (!sAudioReady) {
        return;
    }
    if (sUnderruns != lastUnderruns) {
        printf("AUDIO: %u underruns (ring %u/%u bytes)\n", sUnderruns, osAiGetLength(), (u32) PC_RING_BYTES);
        lastUnderruns = sUnderruns;
    }
}
