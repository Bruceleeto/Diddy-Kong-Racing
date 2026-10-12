#!/usr/bin/env python3
"""Host check: python3 dreamcast/test_aicaflow_controls.py (requires a C compiler).

Compile the production control, song and worker functions with a fake driver.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "dreamcast/audio_aicaflow.c").read_text()


def function(name):
    # Definitions start in column one; an indented call such as "if (!f()) {" does not.
    match = re.search(r"^(?:static )?\w[^\n;{}]*\b" + name + r"\([^;{}]*\) \{", source, re.M)
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
static int sem_wait(semaphore_t *sem) {
    if (!sem->count) longjmp(blocked, 1);
    --sem->count;
    return 0;
}
static int sem_signal(semaphore_t *sem) { ++sem->count; return 0; }
static unsigned sleeps;
static void thd_sleep(int ms) { (void)ms; ++sleeps; }
static uint64_t timer_us_gettime64(void) { return 0; }
void pc_audio_lock(void) {}
void pc_audio_unlock(void) {}
#define benchmark_bank_load(started, bytes) ((void)(started))
#define benchmark_memory(what, id) ((void)0)
#define benchmark_unplanned(id, level) ((void)0)
#define benchmark_sound(id, what, a, b, c) ((void)0)
#define DKR_AFX_RETRY(call) (call)
#define DKR_AFX_JINGLE_COUNT 2u
static const unsigned char dkr_afx_jingle_sequences[DKR_AFX_JINGLE_COUNT] = { 2, 3 };
#define DKR_SONG_DIR "songs"
#define DKR_VOICE_SFX_DIR "voices"
#define DKR_VOICE_RESERVE_BYTES 100u
#define DKR_AFX_VOICE_PACK_COUNT 4u
static const unsigned short dkr_afx_voice_sound_ids[] = { 10, 20, 30 };
static const unsigned short dkr_afx_voice_sound_first[DKR_AFX_VOICE_PACK_COUNT] = { 0, 0, 1, 2 };
static const unsigned short dkr_afx_voice_sound_count[DKR_AFX_VOICE_PACK_COUNT] = { 0, 1, 1, 1 };
static const unsigned int dkr_afx_voice_bytes[DKR_AFX_VOICE_PACK_COUNT] = { 0, 1000, 2000, 3000 };
static unsigned voiceLoads, freeBlock;
int afx_mem_stats(afx_mem_stats_t *memory) { memory->largest_free_block = freeBlock; return 0; }
static unsigned gains, patches, loads, fallbackLoads, bankLoads, bankReleases, frees, stops, recycles;
static int busy, bankResult, noRoom, instanceState = AFX_RUNNING;
static afx_asset_t nextAsset = 1000;
static uint8_t lastGain, loaded[16];
static uint16_t lastValues[AFX_FIELD_COUNT];
int afx_instance_gain(afx_instance_t instance, uint8_t gain) {
    (void)instance; ++gains; lastGain = gain; return busy ? -AFX_BUSY : 0;
}
int afx_instance_patch(afx_instance_t instance, uint8_t channel, uint32_t mask, const uint16_t *values) {
    (void)instance; (void)channel; (void)mask; ++patches;
    memcpy(lastValues, values, 3 * sizeof(*values)); return busy ? -AFX_BUSY : 0;
}
int afx_asset_free(afx_asset_t flow) { (void)flow; ++frees; return 0; }
int afx_bank_load_file(afx_bank_t *bank, const char *path) {
    (void)path; if (bankResult) return bankResult;
    if (noRoom) { --noRoom; return -AFX_NO_AICA_RAM; }
    ++bankLoads; bank->asset = ++nextAsset; bank->bytes = 32; return 0;
}
int afx_bank_release(afx_bank_t *bank) { ++bankReleases; *bank = (afx_bank_t){0}; return 0; }
int afx_update(void) { return 0; }
int afx_instance_status(afx_instance_t instance, afx_instance_status_t *status) {
    (void)instance; status->state = instanceState; return 0;
}
int afx_instance_stop(afx_instance_t instance) { (void)instance; ++stops; instanceState = AFX_DONE; return 0; }
int afx_instance_recycle(afx_instance_t instance) { (void)instance; ++recycles; return 0; }
int afx_instance_activate(afx_asset_t flow, afx_instance_t *instance) {
    *instance = flow; instanceState = AFX_RUNNING; return 0;
}
void sndp_aicaflow_retry_pending(void) {}
int sndp_aicaflow_bank_pending(uint16_t id) { (void)id; return 0; }
'''
globals_ = source[source.index("typedef struct {"):source.index("static afx_asset_t jingle_flow(")]
stubs = r'''
static void update_sfx(void) {}
static void apply_room_control(void) {}
static void apply_controls(void) {}
static int sfx_preempt(uint8_t priority) { (void)priority; return 0; }
static int music_flow_load(const afx_bank_t *bank, const char *directory, uint8_t sequence,
                           afx_asset_t *flow) {
    (void)directory; assert(bank->asset && loads < sizeof(loaded));
    loaded[loads++] = sequence; *flow = ++nextAsset; return 0;
}
static int sfx_bank_release(dkr_sfx_bank_t *bank) { memset(bank, 0, sizeof(*bank)); return 0; }
static int sfx_bank_load(dkr_sfx_bank_t *bank, const char *afb, const char *controls,
                         const uint16_t *ids, uint16_t count) {
    (void)afb; (void)controls; assert(count == 1 && !bank->bank.asset);
    bank->bank.asset = ++nextAsset; freeBlock -= ids[0] * 100u; ++voiceLoads; return 0;
}
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
    "dkr_afx_scene_reverb", "jingle_flow", "music_flow_for", "fallback_bank_active",
    "fallback_quiesce", "fallback_release_idle", "fallback_request", "sfx_loader",
    "song_in_use", "song_release", "song_load", "music_retire",
    "voices_release_one", "voices_load", "dkr_afx_voices_prepare",
    "dkr_afx_music_prepare", "dkr_afx_music_prefetch", "dkr_afx_music_play", "dkr_afx_update",
]
checks = r'''
static void run(void *(*worker)(void *)) {
    if (!setjmp(blocked)) worker(0);
}
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
    /* An idle worker blocks. Multiple queued SFX requests each wake the worker. */
    run(sfx_loader); assert(!fallbackLoads);
    assert(fallback_request(100) == -AFX_BUSY);
    assert(fallback_request(100) == -AFX_BUSY);
    assert(fallback_request(101) == -AFX_BUSY);
    assert(sSfxLoadWake.count == 2);
    run(sfx_loader); assert(fallbackLoads == 2 && !sSfxLoadWake.count && !sleeps);
    /* Jingles are resident: preparing one loads nothing. */
    sReady = 1; sJingleFlows[0] = 2;
    assert(!dkr_afx_music_prepare(2) && !bankLoads && !loads);
    /* A song's samples and flow are resident when prepare returns. */
    assert(!dkr_afx_music_prepare(22));
    assert(bankLoads == 1 && loads == 1 && loaded[0] == 22 && sSong.sequence == 22 && sSong.flow);
    assert(!dkr_afx_music_prepare(22) && bankLoads == 1);
    dkr_afx_music_prefetch(30); assert(bankLoads == 1);
    /* Playing it only activates the resident flow. */
    dkr_afx_music_play(22); dkr_afx_update();
    assert(sMusic == sSong.flow && sCurrentSequence == 22 && bankLoads == 1);
    /* Replacing a playing song stops and recycles it before its samples go. */
    dkr_afx_music_play(23);
    assert(stops == 1 && recycles == 1 && frees == 1 && bankReleases == 1);
    assert(!sMusic && sSong.sequence == 23 && bankLoads == 2 && loaded[1] == 23);
    dkr_afx_update(); assert(sMusic == sSong.flow && sCurrentSequence == 23);
    /* A jingle replaces the playing instance but keeps the song resident. */
    dkr_afx_music_play(2);
    dkr_afx_update(); dkr_afx_update(); dkr_afx_update();
    assert(sCurrentSequence == 2 && sSong.sequence == 23 && bankLoads == 2 && bankReleases == 1);
    /* Loading a song under a playing jingle leaves the jingle alone. */
    assert(!dkr_afx_music_prepare(24));
    assert(stops == 2 && sMusic && sCurrentSequence == 2 && sSong.sequence == 24);
    /* A song that does not fit leaves no half-loaded song behind. */
    bankResult = -AFX_NO_AICA_RAM;
    assert(dkr_afx_music_prepare(25) == -AFX_NO_AICA_RAM);
    assert(!sSong.flow && !sSong.bank.asset && !sSong.sequence);
    /* Voice packs load in the order given while each one leaves the reserve.
     * Character 0 has no pack, 9 does not exist and 2 is listed twice. */
    freeBlock = 3200;
    dkr_afx_voices_prepare((const uint8_t[]){ 2, 0, 9, 2, 1, 3 }, 6);
    assert(voiceLoads == 2 && sVoiceSfx[2].bank.asset && sVoiceSfx[1].bank.asset && !sVoiceSfx[3].bank.asset);
    /* Space is taken back from the least important pack, never a playing one. */
    assert(voices_release_one() && !sVoiceSfx[1].bank.asset && sVoiceSfx[2].bank.asset);
    sSfxSlots[0] = (dkr_sfx_slot_t){ .instance = 1, .bank = &sVoiceSfx[2] };
    assert(!voices_release_one() && sVoiceSfx[2].bank.asset);
    sSfxSlots[0].instance = 0;
    /* A song that needs a voice pack's space gets it. */
    bankResult = 0; noRoom = 2;
    assert(!dkr_afx_music_prepare(26) && sSong.sequence == 26 && !sVoiceSfx[2].bank.asset);
    /* The next race drops the packs it has no racer for. */
    freeBlock = 5000; voiceLoads = 0;
    dkr_afx_voices_prepare((const uint8_t[]){ 2 }, 1);
    freeBlock = 5000; /* The fake allocator does not refund a release. */
    dkr_afx_voices_prepare((const uint8_t[]){ 3 }, 1);
    assert(voiceLoads == 2 && !sVoiceSfx[2].bank.asset && sVoiceSfx[3].bank.asset);
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
print("AICAflow controls, songs and worker wakeups: OK")
