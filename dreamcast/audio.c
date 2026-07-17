// Dreamcast audio backend — real AICA output via KOS snd_stream.
//
// This is the ONLY platform-specific audio file. Everything above it — the
// libultra ALSynth sequencer/synthesizer, the audio manager, the (M3)
// command-list interpreter (pc_audio_submit in src/audiomgr.c) — is portable C
// and produces real interleaved stereo s16 PCM at 22050 Hz. This file takes the
// PCM handed to osAiSetNextBuffer and feeds it to the AICA through a KOS sound
// stream; earlier milestones threw it away ("silent but correctly paced").
//
// Two independent jobs live here, deliberately kept separate:
//
//   1. Pacing (unchanged from the silent build). The audio manager runs a
//      frame-size feedback loop: __amHandleFrameMsg recomputes how many samples
//      to synthesize each frame from osAiGetLength(). That loop is stable and is
//      NOT tied to real hardware — it models a virtual output queue by byte count
//      (sQueued) drained one video frame at a time in pc_audio_frame(). We leave
//      it exactly as it was so synthesis keeps producing ~1 frame of audio per
//      tick regardless of what AICA is doing.
//
//   2. Output (new). osAiSetNextBuffer deinterleaves each produced frame into two
//      per-channel ring buffers; a KOS snd_stream direct callback pulls from them
//      to fill AICA's SPU buffers, and pc_audio_frame() polls the stream once per
//      video frame. If AICA init fails we simply never start the stream and the
//      build degrades to the old silent-but-paced behaviour — the game still runs.
//
// The ring is bounded (RING_BYTES/ch). Synthesis is paced to roughly the output
// rate and AICA consumes at exactly the output rate, so the two stay balanced;
// transient over/underruns drop/zero a little audio and self-correct rather than
// stalling anything.

#include <kos.h>
#include <stddef.h> // wchar_t, before <dc/sound/stream.h> pulls <inttypes.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <string.h>

typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t u8;
typedef uint32_t u32;

// The audio manager's per-frame tick (src/audiomgr.c, TARGET_PC path).
extern void am_audio_frame_pc(void);

// src/audiomgr.c: samples of audio the manager wants to produce per tick. Derived
// from outputRate * 2 / refreshRate = 735 at NTSC — i.e. 1/30s of audio, because
// the audio thread ticks once per game frame (two video fields), not per field.
extern unsigned int frameSize;

#define DC_AUDIO_RATE 22050
#define DC_AUDIO_BYTES_PER_SAMPLE 4 // stereo s16 (interleaved, as the manager sees it)

// Everything below runs on the main thread only: osAiSetNextBuffer, osAiGetLength
// and pc_audio_frame are all called from the frame pump, and the snd_stream direct
// callback fires synchronously from inside snd_stream_poll() (also on this thread).
// So no locking is needed between the ring writer and the ring reader.

// ---------------------------------------------------------------------------
// Pacing model (identical to the silent build — do not entangle with output)
// ---------------------------------------------------------------------------

static u32 sQueued; // bytes "submitted to the DAC" and not yet drained (virtual)
static s32 sReady;

// ---------------------------------------------------------------------------
// Real output: per-channel ring buffers + KOS sound stream
// ---------------------------------------------------------------------------

#define RING_BYTES 32768 // per channel; ~0.74s at 22050 Hz, 16-bit mono
#define SND_STREAM_BUFSIZE 4096

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

typedef struct {
    u8 *buf;
    u32 cap; // power of two
    u32 head;
    u32 tail;
} ring_t;

static u8 __attribute__((aligned(32))) sRingStorage[2][RING_BYTES];
static ring_t sRing[2];

static volatile snd_stream_hnd_t sStream = SND_STREAM_INVALID;
static s32 sAudioOk;     // AICA/stream initialised successfully
static s32 sStreamStarted;

static void ring_init(int n) {
    // Round capacity up to a power of two so head/tail can free-run and wrap with
    // a mask (RING_BYTES is already a power of two, this is just belt-and-braces).
    sRing[n].cap = 1u << (32 - __builtin_clz(RING_BYTES - 1));
    sRing[n].buf = sRingStorage[n];
    sRing[n].head = 0;
    sRing[n].tail = 0;
}

static void ring_write(int n, const void *src, u32 count) {
    ring_t *r = &sRing[n];
    u32 mask = r->cap - 1;
    u32 free = r->cap - (r->head - r->tail);
    u32 idx, first;

    if (count > free) {
        return; // overrun: drop this chunk, pacing loop will resettle
    }
    idx = r->head & mask;
    first = MIN(count, r->cap - idx);
    memcpy(r->buf + idx, src, first);
    if (count - first) {
        memcpy(r->buf, (const u8 *) src + first, count - first);
    }
    r->head += count;
}

static void ring_read(int n, void *dst, u32 count) {
    ring_t *r = &sRing[n];
    u32 mask = r->cap - 1;
    u32 avail = r->head - r->tail;
    u32 idx, first;

    if (count > avail) {
        memset(dst, 0, count); // underrun: emit silence
        return;
    }
    idx = r->tail & mask;
    first = MIN(count, r->cap - idx);
    memcpy(dst, r->buf + idx, first);
    if (count - first) {
        memcpy((u8 *) dst + first, r->buf, count - first);
    }
    r->tail += count;
}

// KOS direct callback: AICA wants more samples. size_req is the TOTAL byte count
// across both channels (snd_stream_fill passes size*channels), so each channel
// gets half. left/right point straight at SPU RAM.
static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t left, uintptr_t right, size_t size_req) {
    (void) hnd;
    ring_read(0, (void *) left, size_req >> 1);
    ring_read(1, (void *) right, size_req >> 1);
    return size_req;
}

// ---------------------------------------------------------------------------
// AI shims (replacing libultra/src/io/ai.c, aigetlen.c, aisetfreq.c,
// aisetnextbuf.c — all dropped from the build, they only poke MMIO)
// ---------------------------------------------------------------------------

// Called once from amCreateAudioMgr with OUTPUT_RATE. The manager derives its
// whole frame-size schedule from the value we return, so it must be the rate we
// actually pace (and now play) against. We also bring AICA up here; failure is
// non-fatal — the build just stays silent.
s32 osAiSetFrequency(u32 frequency) {
    (void) frequency;
    sReady = 1;

    if (!sAudioOk) {
        ring_init(0);
        ring_init(1);

        if (snd_stream_init() == 0) {
            sStream = snd_stream_alloc(NULL, SND_STREAM_BUFSIZE);
            if (sStream != SND_STREAM_INVALID) {
                snd_stream_set_callback_direct(sStream, audio_cb);
                snd_stream_volume(sStream, 255);
                sAudioOk = 1;
            } else {
                printf("DC Audio: snd_stream_alloc failed; running silent\n");
            }
        } else {
            printf("DC Audio: snd_stream_init failed; running silent\n");
        }
    }

    return (s32) DC_AUDIO_RATE;
}

// The game hands us the PCM the mixer produced for the previous frame: interleaved
// stereo s16. Deinterleave it into the two channel rings (output), and account the
// byte count so the pacing loop stays honest (unchanged from the silent build).
void osAiSetNextBuffer(void *buf, u32 size) {
    if (!sReady || size == 0) {
        return;
    }

    sQueued += size; // pacing signal — see osAiGetLength()

    if (sAudioOk && buf != NULL) {
        const s16 *pcm = (const s16 *) buf;
        u32 frames = size / DC_AUDIO_BYTES_PER_SAMPLE; // stereo sample pairs
        u32 done = 0;

        // Deinterleave in bounded chunks so the scratch stays on the stack.
        while (done < frames) {
            s16 l[256];
            s16 r[256];
            u32 n = MIN(frames - done, 256u);
            u32 i;

            for (i = 0; i < n; i++) {
                l[i] = pcm[(done + i) * 2 + 0];
                r[i] = pcm[(done + i) * 2 + 1];
            }
            ring_write(0, l, n * sizeof(s16));
            ring_write(1, r, n * sizeof(s16));
            done += n;
        }

        if (!sStreamStarted) {
            sStreamStarted = 1;
            snd_stream_start(sStream, DC_AUDIO_RATE, 1 /* stereo */);
        }
    }
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
// the hardware, and the formula always lands in [112, 848]. This models the
// virtual queue only — it is intentionally NOT the real ring depth, so pacing is
// decoupled from AICA's actual drain.
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
// Pacing (unchanged): drain one video frame of the virtual queue, then top the
// queue back up to a target depth by synthesizing more, bounded so a bad state
// can't spin forever. am_audio_frame_pc() feeds osAiSetNextBuffer, which also
// fills the output rings as a side effect.
//
// Output: poll the sound stream so its callback can refill AICA from the rings.
#define DC_AUDIO_TARGET_FRAMES 3 // ~100ms of buffered audio
#define DC_AUDIO_MAX_TICKS 6     // don't spin forever if something goes wrong

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

    // Hand whatever the rings now hold to AICA.
    if (sAudioOk && sStreamStarted) {
        snd_stream_poll(sStream);
    }
}

// Diagnostics hook kept for API parity with the main loop.
void pc_audio_report(void) {
}
