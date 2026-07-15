// Dreamcast audio backend — silent, but correctly paced.
//
// This is the ONLY platform-specific audio file. Everything above it — the
// libultra ALSynth sequencer/synthesizer, the audio manager, the (M3)
// command-list interpreter (pc_audio_submit in src/audiomgr.c) — is portable C.
//
// There is no SDL and no DAC output here yet: pc_audio_submit already zeroes the
// synth output buffer, so nothing this file does would be audible anyway. What
// this file MUST still do is keep the audio manager's frame-size feedback loop
// alive and stable, because the game ticks the whole audio subsystem through it:
//
//   1. The AI (Audio Interface) shims. On N64 these poke MMIO registers. Here
//      they model a virtual output queue by byte count only — the PCM itself is
//      discarded. osAiGetLength() is NOT a throwaway stub: __amHandleFrameMsg
//      recomputes how many samples to synthesize each frame from it, and if it
//      lies the frame-size loop goes unstable (see the saturation note below).
//
//   2. The frame pump. No audio thread, no scheduler (see dreamcast/reimpl.c),
//      so the audio manager is ticked once per video frame from the main loop.
//
// When real AICA/snd_stream output is wired up, only this file changes: feed the
// PCM handed to osAiSetNextBuffer into a KOS snd_stream callback and let the
// stream's real drain replace the synthetic one in pc_audio_frame().

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

#define DC_AUDIO_RATE 22050
#define DC_AUDIO_BYTES_PER_SAMPLE 4 // stereo s16

// Everything below runs on the main thread only (no audio callback thread), so
// no locking is needed. sQueued is a pure byte count standing in for the depth
// of the output DAC; the audio samples themselves are thrown away.
static u32 sQueued; // bytes "submitted to the DAC" and not yet drained
static s32 sReady;

// ---------------------------------------------------------------------------
// AI shims (replacing libultra/src/io/ai.c, aigetlen.c, aisetfreq.c,
// aisetnextbuf.c — all dropped from the build, they only poke MMIO)
// ---------------------------------------------------------------------------

// Called once from amCreateAudioMgr with OUTPUT_RATE. The manager derives its
// whole frame-size schedule from the value we return, so it must be the rate we
// actually pace against.
s32 osAiSetFrequency(u32 frequency) {
    (void) frequency;
    sReady = 1;
    return (s32) DC_AUDIO_RATE;
}

// The game hands us the PCM the mixer produced for the previous frame. Silent
// build: account the byte count so osAiGetLength/pacing stay honest, drop the data.
void osAiSetNextBuffer(void *buf, u32 size) {
    (void) buf;
    if (!sReady || size == 0) {
        return;
    }
    sQueued += size;
}

// Bytes still to play. This is the feedback signal __amHandleFrameMsg uses:
//
//     frameSamples = (16 + (frameSize - osAiGetLength()/4 + 96)) & ~0xf
//
// On N64 the AI holds at most two buffers — one playing, one pending — and
// osAiGetLength() returns what is left of the playing one, never more than a
// single frame. Reporting the whole backlog instead makes (frameSize - samplesLeft)
// go NEGATIVE; the clamp tests `(u32) info->frameSamples < minFrameSize`, so a
// negative value casts to a huge unsigned, sails through the clamp, and
// osAiSetNextBuffer gets a ~4GB length. So: saturate at one frame, exactly like
// the hardware, and the formula always lands in [112, 848].
u32 osAiGetLength(void) {
    u32 oneFrame;

    if (frameSize == 0) {
        return sQueued; // before amCreateAudioMgr has run
    }
    oneFrame = frameSize * DC_AUDIO_BYTES_PER_SAMPLE;
    return (sQueued > oneFrame) ? oneFrame : sQueued;
}

// ---------------------------------------------------------------------------
// Frame pump
// ---------------------------------------------------------------------------

// Called once per video frame from dreamcast/main.c, standing in for the audio
// thread's OS_SC_RETRACE_MSG wakeup.
//
// With no real DAC to drain the queue, we drain it synthetically: one video
// frame has elapsed, so one video-frame worth of queued audio has now "played".
// Then top the queue back up to a target depth, bounded so a bad state can't
// spin forever. This is the same self-correcting pacing the SDL backend used,
// just with the ring-buffer callback replaced by the drain below — pace against
// the queue, never against the host frame rate.
#define DC_AUDIO_TARGET_FRAMES 3 // ~100ms of buffered audio
#define DC_AUDIO_MAX_TICKS 4     // don't spin forever if something goes wrong

void pc_audio_frame(void) {
    u32 drained;
    u32 target;
    s32 ticks = 0;

    if (!sReady || frameSize == 0) {
        return;
    }

    // Synthetic DAC drain: retire one video frame of queued audio.
    drained = frameSize * DC_AUDIO_BYTES_PER_SAMPLE;
    sQueued = (sQueued > drained) ? sQueued - drained : 0;

    target = frameSize * DC_AUDIO_BYTES_PER_SAMPLE * DC_AUDIO_TARGET_FRAMES;
    while (sQueued < target && ticks < DC_AUDIO_MAX_TICKS) {
        am_audio_frame_pc();
        ticks++;
    }
}

// Diagnostics hook kept for API parity with the main loop; nothing to report in
// the silent backend.
void pc_audio_report(void) {
}
