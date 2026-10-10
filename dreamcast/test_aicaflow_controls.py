#!/usr/bin/env python3
"""Host check: python3 dreamcast/test_aicaflow_controls.py (requires a C compiler).

Compile the production control/worker functions with fake IPC and semaphores.
The semaphore hook injects events exactly where a worker would block.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "dreamcast/audio_aicaflow.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?[^\n;{}]*\b" + name + r"\([^;{}]*\) \{", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <aicaflow/host.h>
#include <aicaflow/bank.h>
typedef struct { unsigned count; } semaphore_t;
#define SEM_INITIALIZER(n) { n }
static jmp_buf blocked;
static void (*on_wait)(void);
static int sem_wait(semaphore_t *sem) {
    if (!sem->count && on_wait) {
        void (*event)(void) = on_wait;
        on_wait = 0;
        event();
    }
    if (!sem->count) longjmp(blocked, 1);
    --sem->count;
    return 0;
}
static int sem_signal(semaphore_t *sem) { ++sem->count; return 0; }
static void thd_sleep(int ms) { (void)ms; assert(!"worker polled instead of blocking"); }
static unsigned gains, patches, loads, fallbackLoads;
static int busy;
static uint8_t lastGain, loaded[16];
static uint16_t lastValues[AFX_FIELD_COUNT];
int afx_instance_gain(afx_instance_t instance, uint8_t gain) {
    (void)instance; ++gains; lastGain = gain; return busy ? -AFX_BUSY : 0;
}
int afx_instance_patch(afx_instance_t instance, uint8_t channel, uint32_t mask, const uint16_t *values) {
    (void)instance; (void)channel; (void)mask; ++patches;
    memcpy(lastValues, values, 3 * sizeof(*values)); return busy ? -AFX_BUSY : 0;
}
int afx_asset_free(afx_asset_t flow) { (void)flow; return 0; }
int afx_update(void) { return 0; }
int afx_instance_status(afx_instance_t instance, afx_instance_status_t *status) {
    (void)instance; status->state = AFX_RUNNING; return 0;
}
int afx_instance_stop(afx_instance_t instance) { (void)instance; return 0; }
int afx_instance_recycle(afx_instance_t instance) { (void)instance; return 0; }
int afx_instance_activate(afx_asset_t flow, afx_instance_t *instance) { *instance = flow; return 0; }
void sndp_aicaflow_retry_pending(void) {}
int sndp_aicaflow_bank_pending(uint16_t id) { (void)id; return 0; }
'''
globals_ = source[source.index("typedef struct {"):source.index("static afx_asset_t short_music_flow(")]
stubs = r'''
static void update_sfx(void) {}
static void apply_room_control(void) {}
static void apply_controls(void) {}
static int sfx_preempt(uint8_t priority) { (void)priority; return 0; }
static int music_flow_load(uint8_t sequence, afx_asset_t *flow) {
    assert(loads < sizeof(loaded)); loaded[loads++] = sequence; *flow = sequence; return 0;
}
static int sfx_bank_release(dkr_sfx_bank_t *bank) { memset(bank, 0, sizeof(*bank)); return 0; }
static void fallback_load(void) {
    for (unsigned i = 0; i < DKR_FALLBACK_SLOTS; ++i) {
        if (sFallback[i].requested) {
            sFallback[i].requested = 0; ++fallbackLoads; return;
        }
    }
}
'''
names = [
    "sfx_slot", "scaled_pitch", "panned_direct", "apply_sfx_controls",
    "dkr_afx_sfx_start_controls", "dkr_afx_sfx_volume", "dkr_afx_sfx_pitch",
    "dkr_afx_sfx_pan", "dkr_afx_sfx_fx", "dkr_afx_music_gain", "dkr_afx_music_tempo",
    "dkr_afx_music_lane_mute", "dkr_afx_music_lane_volume", "dkr_afx_music_lane_fade",
    "dkr_afx_scene_reverb", "short_music_flow", "music_flow_for", "fallback_bank_active",
    "fallback_request", "music_load_enqueue", "sfx_loader", "music_loader",
    "dkr_afx_music_prepare", "dkr_afx_music_prefetch", "collect_loaded_music", "dkr_afx_update",
]
checks = r'''
static void run(void *(*worker)(void *)) {
    if (!setjmp(blocked)) worker(0);
}
static void consume_music(void) { collect_loaded_music(); }
static void enqueue_at_wait(void) { music_load_enqueue(24, 0); }
int main(void) {
    uint16_t fields[AFX_FIELD_COUNT] = {0};
    dkr_sfx_sound_t sound = { .channels = 1, .fields = fields };
    dkr_sfx_slot_t *slot = &sSfxSlots[0];
    *slot = (dkr_sfx_slot_t){ .owner = &sound, .sound = &sound,
        .instance = 1, .initializing = 1, .dirty = 15, .volume = 255, .pitch = 1, .pan = 64 };
    /* One gain and one combined patch, with the final initial values. */
    dkr_afx_sfx_start_controls(&sound, 80, 1.5f, 100, 4);
    assert(gains == 1 && patches == 1 && lastGain == 80 && !slot->dirty);
    assert(lastValues[0] == scaled_pitch(0, 1.5f));
    assert(lastValues[2] == panned_direct(0, 100));
    dkr_afx_sfx_volume(&sound, 80); dkr_afx_sfx_pitch(&sound, 1.5f);
    dkr_afx_sfx_pan(&sound, 100); dkr_afx_sfx_fx(&sound, 4);
    assert(gains == 1 && patches == 1);
    /* A failed initial publication remains dirty even for an identical setter. */
    busy = 1; dkr_afx_sfx_volume(&sound, 81);
    assert(slot->dirty == DKR_SFX_DIRTY_VOLUME);
    busy = 0; dkr_afx_sfx_volume(&sound, 81);
    assert(!slot->dirty && lastGain == 81);
    slot->initializing = 0;
    dkr_afx_sfx_pitch(&sound, 2); dkr_afx_sfx_pitch(&sound, 2);
    assert(slot->dirty == DKR_SFX_DIRTY_PITCH);
    apply_sfx_controls(slot); assert(!slot->dirty);
    dkr_afx_music_gain(255); dkr_afx_music_tempo(120);
    dkr_afx_music_lane_mute(2, 0); dkr_afx_music_lane_volume(2, 127);
    dkr_afx_music_lane_fade(2, 127); dkr_afx_scene_reverb(1);
    assert(!sControlDirty && !sMuteDirty && !sVolumeDirty && !sRoomDirty);
    dkr_afx_music_gain(90); dkr_afx_music_tempo(140);
    dkr_afx_music_lane_mute(2, 2); dkr_afx_music_lane_volume(2, 80);
    dkr_afx_music_lane_fade(3, 60); dkr_afx_scene_reverb(0);
    assert(sControlDirty == 3 && sMuteDirty == 4 && sVolumeDirty == 12 && sRoomDirty == 1);
    dkr_afx_music_gain(90); dkr_afx_music_tempo(140);
    dkr_afx_music_lane_volume(2, 80); dkr_afx_music_lane_fade(3, 60); dkr_afx_scene_reverb(0);
    assert(sControlDirty == 3 && sVolumeDirty == 12 && sRoomDirty == 1);
    sMuteDirty = 0; dkr_afx_music_lane_mute(2, 1); assert(!sMuteDirty);
    dkr_afx_music_lane_mute(2, 0); assert(sMuteDirty == 4);
    sVolumeDirty = 0; dkr_afx_music_lane_volume(16, 0); assert(!sVolumeDirty);
    /* Idle workers block. Multiple queued SFX requests each wake the worker. */
    run(sfx_loader); assert(!fallbackLoads);
    assert(fallback_request(100) == -AFX_BUSY);
    assert(fallback_request(100) == -AFX_BUSY);
    assert(fallback_request(101) == -AFX_BUSY);
    assert(sSfxLoadWake.count == 2);
    run(sfx_loader); assert(fallbackLoads == 2 && !sSfxLoadWake.count);
    run(music_loader); assert(!loads);
    /* Latest request wins; prepared music has priority over prefetch. */
    sReady = 1;
    dkr_afx_music_prefetch(20); dkr_afx_music_prefetch(21);
    assert(!dkr_afx_music_prepare(22));
    run(music_loader); assert(loads == 1 && loaded[0] == 22 && sLoadedMusicReady);
    collect_loaded_music(); assert(!sLoadedMusicReady && sPrefetchedMusicSequence == 22);
    /* Promotion releases the prefetch priority gate and wakes the worker. */
    run(music_loader); assert(!sMusicLoadWake.count && loads == 1);
    dkr_afx_update(); assert(!sPreparedSequence && sMusicLoadWake.count);
    run(music_loader); assert(loads == 2 && loaded[1] == 21);
    /* A second completed load waits for the first result to be consumed. */
    dkr_afx_music_prefetch(23);
    on_wait = consume_music;
    run(music_loader);
    assert(loads == 3 && loaded[2] == 23 && sLoadedMusicReady && sLoadedSequence == 23);
    collect_loaded_music(); run(music_loader); assert(!sLoadedMusicReady);
    /* Enqueue between the empty-queue check and wait must not lose its wakeup. */
    sPrefetchSequence = 24; on_wait = enqueue_at_wait;
    run(music_loader); assert(loads == 4 && loaded[3] == 24 && sLoadedMusicReady);
    collect_loaded_music(); run(music_loader);
    /* Selecting an already-resident song also releases that gate. */
    sPreparedSequence = 22; dkr_afx_music_prefetch(25); run(music_loader);
    assert(loads == 4);
    sShortMusicFlows[0] = 2; assert(!dkr_afx_music_prepare(2));
    assert(sMusicLoadWake.count);
    run(music_loader); assert(loads == 5 && loaded[4] == 25);
    collect_loaded_music(); run(music_loader);
    /* No-RAM retry wakes the loader after releasing the previous music flow. */
    sMusicFlow = 10; sMusicFlowSequence = 10; sPreparedSequence = 26;
    sLoadedSequence = 26; sLoadedMusicResult = -AFX_NO_AICA_RAM; sLoadedMusicReady = 1;
    collect_loaded_music(); run(music_loader);
    assert(loads == 6 && loaded[5] == 26 && !sMusicFlow);
    return 0;
}
'''
defines = "\n".join(re.findall(r"^#define DKR_\w+ [0-9]+$", source, re.M))
with tempfile.TemporaryDirectory(prefix="dkr-audio-check-") as directory:
    path = Path(directory)
    (path / "check.c").write_text(prefix + defines + "\n" + globals_ + stubs +
                                  "\n".join(function(name) for name in names) + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c2x", "-O2",
                    "-I" + str(root / "third_party/aicaflow/driver/sh4/include"),
                    "-I" + str(root / "third_party/aicaflow/driver/include"),
                    "-I" + str(root / "third_party/aicaflow/driver/format/include"),
                    str(path / "check.c"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True, timeout=10)
print("AICAflow controls and loader wakeups: OK")
