#include "dkr_asset_mount.h"
#include "audio_aicaflow.h"
#include "sound_ids.h"

#include <aicaflow/host.h>
#include <aicaflow/dsp.h>
#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include "sfx_manifest.h"
#include "music_manifest.h"
#include <fcntl.h>
#include <kos.h>
#include <malloc.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DKR_SEQUENCE_NONE 0
#define DKR_SEQUENCE_NONE2 1
#define DKR_STARTUP_MUSIC 35
#define DKR_SFX_SLOTS 16
#define DKR_SFX_DIRTY_VOLUME 1
#define DKR_SFX_DIRTY_PITCH 2
#define DKR_SFX_DIRTY_PAN 4
#define DKR_SFX_DIRTY_FX 8

#ifndef DKR_AICAFLOW_BENCHMARK
#define DKR_AICAFLOW_BENCHMARK 0
#endif

#if DKR_AICAFLOW_BENCHMARK
enum { DKR_AFX_BENCHMARK_US = 120u * 1000u * 1000u };
static uint64_t sBenchStarted;
static uint32_t sBenchUpdates, sBenchUpdateUs, sBenchFlowUploads, sBenchFlowUploadUs;
static uint32_t sBenchFlowUploadBytes, sBenchBankLoads, sBenchBankLoadUs, sBenchBankLoadBytes;
static uint32_t sBenchHeapStart, sBenchHeapPeak;

static uint32_t benchmark_heap_used(void) {
    struct mallinfo heap = mallinfo();
    return heap.uordblks > UINT32_MAX ? UINT32_MAX : (uint32_t)heap.uordblks;
}

static void benchmark_reset(void) {
    sBenchStarted = timer_us_gettime64();
    sBenchHeapStart = sBenchHeapPeak = benchmark_heap_used();
    __atomic_store_n(&sBenchUpdates, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchUpdateUs, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchFlowUploads, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchFlowUploadUs, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchFlowUploadBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchBankLoads, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchBankLoadUs, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sBenchBankLoadBytes, 0, __ATOMIC_RELAXED);
}

static void benchmark_add(volatile uint32_t *total, uint64_t started) {
    uint64_t elapsed = timer_us_gettime64() - started;
    __atomic_fetch_add(total, elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed, __ATOMIC_RELAXED);
}

static void benchmark_flow_upload(uint64_t started, uint32_t bytes) {
    __atomic_fetch_add(&sBenchFlowUploads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sBenchFlowUploadBytes, bytes, __ATOMIC_RELAXED);
    benchmark_add(&sBenchFlowUploadUs, started);
}

static void benchmark_bank_load(uint64_t started, uint32_t bytes) {
    __atomic_fetch_add(&sBenchBankLoads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sBenchBankLoadBytes, bytes, __ATOMIC_RELAXED);
    benchmark_add(&sBenchBankLoadUs, started);
}

/* Shows how each planned load left the arena, and which sounds no plan lists. */
static void benchmark_memory(const char *what, unsigned id) {
    afx_mem_stats_t memory;
    if (afx_mem_stats(&memory)) return;
    printf("DKR AICAFLOW BENCH: %s %u loaded, free=%lu largest=%lu blocks=%lu\n", what, id,
           (unsigned long)memory.free_bytes, (unsigned long)memory.largest_free_block,
           (unsigned long)memory.free_block_count);
}

static void benchmark_init(uint64_t started) {
    printf("DKR AICAFLOW BENCH: afx_init %lu us\n", (unsigned long)(timer_us_gettime64() - started));
}

static void benchmark_unplanned(unsigned id, unsigned level) {
    printf("DKR AICAFLOW BENCH: unplanned sound %u in level %u\n", id, level);
}

static void benchmark_sound(unsigned id, const char *what, int a, int b, int c);

static void benchmark_update(uint64_t started) {
    static uint32_t lateness;
    uint32_t worst = afx_status_max_lateness();
    if (worst > lateness) {
        printf("DKR AICAFLOW BENCH: lateness %lu -> %lu ticks\n", (unsigned long)lateness, (unsigned long)worst);
        lateness = worst;
    }
    uint64_t now = timer_us_gettime64();
    uint32_t heap = benchmark_heap_used();
    __atomic_fetch_add(&sBenchUpdates, 1, __ATOMIC_RELAXED);
    benchmark_add(&sBenchUpdateUs, started);
    if (heap > sBenchHeapPeak) sBenchHeapPeak = heap;
    if (now - sBenchStarted < DKR_AFX_BENCHMARK_US) return;
    char result[288];
    int bytes = snprintf(result, sizeof(result),
                         "DKR AICAFLOW BENCH: wall=%llu ms update=%lu us/%lu calls flow=%lu us/%lu calls/%lu bytes bank=%lu us/%lu calls/%lu bytes sh4_heap=%lu/%lu/%lu bytes lateness=%lu ticks\n",
                         (unsigned long long)((now - sBenchStarted) / 1000u),
                         __atomic_load_n(&sBenchUpdateUs, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchUpdates, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchFlowUploadUs, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchFlowUploads, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchFlowUploadBytes, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchBankLoadUs, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchBankLoads, __ATOMIC_RELAXED),
                         __atomic_load_n(&sBenchBankLoadBytes, __ATOMIC_RELAXED),
                         (unsigned long)sBenchHeapStart,
                         (unsigned long)sBenchHeapPeak,
                         (unsigned long)heap,
                         (unsigned long)afx_status_max_lateness());
    printf("%s", result);
    file_t file = fs_open(DKR_ASSET_MOUNT "/build/dc/aicaflow/benchmark.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (file != FILEHND_INVALID) {
        (void)fs_write(file, result, bytes);
        (void)fs_close(file);
    }
    benchmark_reset();
}

/* The driver starts an instance one tick after the AICA clock it reads. If
 * the command then reaches the queue later than that, the instance starts
 * late by the difference, which the firmware reports as lateness. */
static int benchmark_activate(afx_asset_t flow, afx_instance_t *instance, const char *what, unsigned id) {
    uint32_t before = afx_status_timer_ticks();
    uint64_t started = timer_us_gettime64();
    int result = afx_instance_activate(flow, instance);
    uint32_t us = (uint32_t)(timer_us_gettime64() - started);
    uint32_t ticks = afx_status_timer_ticks() - before;
    if (ticks > 1 || us > 1000)
        printf("DKR AICAFLOW BENCH: activating %s %u took %lu us, %lu ticks (%d)\n", what, id,
               (unsigned long)us, (unsigned long)ticks, result);
    return result;
}

/* A finished instance reports the worst lateness of its own events. */
static void benchmark_late(const char *what, unsigned id, const afx_instance_status_t *status) {
    if (status->state == AFX_DONE && !status->result && status->detail >= 4)
        printf("DKR AICAFLOW BENCH: %s %u ran up to %lu ticks late\n", what, id, (unsigned long)status->detail);
}
#else
#define benchmark_activate(flow, instance, what, id) afx_instance_activate(flow, instance)
#define benchmark_late(what, id, status) ((void)0)
#define benchmark_reset() ((void)0)
#define benchmark_flow_upload(started, bytes) ((void)0)
#define benchmark_bank_load(started, bytes) ((void)0)
#define benchmark_memory(what, id) ((void)0)
#define benchmark_init(started) ((void)(started))
#define benchmark_unplanned(id, level) ((void)0)
#define benchmark_sound(id, what, a, b, c) ((void)0)
#define benchmark_update(started) ((void)0)
#endif

alignas(32) static const unsigned char firmware[] = {
#embed "../third_party/aicaflow/firmware/aicaflow.drv"
};

#define DKR_RESIDENT_SFX_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/core.afb"
#define DKR_SCENE_SFX_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/scenes"
#define DKR_VEHICLE_SFX_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/vehicle.afb"
#define DKR_VOICE_SFX_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/voices"
/* Kept free beside the voice packs for sounds that no plan lists. */
#define DKR_VOICE_RESERVE_BYTES 0x10000u
#define DKR_JINGLE_BANK_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/jingles.afb"
#define DKR_JINGLE_CONTROL_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/jingle_controls"
#define DKR_SONG_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/music_raw"
#define DKR_SEQUENCE_COUNT 66

/* Planned loads run with the game audio mutex held and the fallback worker
 * idle, so a busy driver lock is only a momentary preemption. The driver drops
 * a recycled instance's reference to its flow on a later afx_update(), so a
 * release that follows a recycle waits for that too. */
#define DKR_AFX_RETRY(call) ({ \
    int retry_result; \
    unsigned retry_count = 0; \
    while (((retry_result = (call)) == -AFX_BUSY || retry_result == -AFX_ASSET_REFERENCED) && \
           ++retry_count < 250) { \
        if (retry_result == -AFX_ASSET_REFERENCED) (void)afx_update(); \
        thd_sleep(1); \
    } \
    retry_result; \
})

typedef struct {
    uint16_t id;
    uint16_t channels;
    afx_asset_t flow;
    uint16_t *fields;
} dkr_sfx_sound_t;

typedef struct {
    afx_bank_t bank;
    dkr_sfx_sound_t *sounds;
    uint16_t sound_count;
} dkr_sfx_bank_t;

/* A song owns its samples. Only the playing song and the jingles are resident. */
typedef struct {
    afx_bank_t bank;
    afx_asset_t flow;
    uint8_t sequence;
} dkr_song_t;

typedef struct {
    void *owner;
    afx_instance_t instance;
    const dkr_sfx_bank_t *bank;
    const dkr_sfx_sound_t *sound;
    uint32_t dirty;
    uint8_t volume;
    float pitch;
    uint8_t pan, fx, priority, initializing, stopping;
} dkr_sfx_slot_t;

extern void sndp_aicaflow_complete(void *owner);
extern void sndp_aicaflow_preempt(void *owner);
extern void sndp_aicaflow_retry_pending(void);
extern int sndp_aicaflow_bank_pending(uint16_t id);
extern int sndp_aicaflow_voice_available(void *owner);
static void apply_sfx_controls(dkr_sfx_slot_t *slot);
static void update_sfx(void);
static void *sfx_loader(void *unused);
static int music_flow_load(const afx_bank_t *bank, const char *directory, uint8_t sequence,
                           afx_asset_t *out_flow);
static int song_load(uint8_t sequence);
static int song_release(void);
static int song_in_use(void);
static int music_retire(void);
static int fallback_request(uint16_t id);
static void fallback_load(void);
static int fallback_clear(void);
static void fallback_quiesce(void);
static void fallback_release_idle(unsigned keep);
static int fallback_bank_active(const dkr_sfx_bank_t *bank);
static int sfx_bank_release(dkr_sfx_bank_t *bank);
extern void pc_audio_lock(void);
extern void pc_audio_unlock(void);

static dkr_song_t sSong;
static afx_bank_t sJingleBank;
static afx_asset_t sJingleFlows[DKR_AFX_JINGLE_COUNT];
static afx_instance_t sMusic;
static dkr_sfx_slot_t sSfxSlots[DKR_SFX_SLOTS];
static dkr_sfx_bank_t sResidentSfx;
static dkr_sfx_bank_t sSceneSfx;
static dkr_sfx_bank_t sVehicleSfx;
/* One pack per racer character. The level's packs and its song come first;
 * these take what is left and give it back when a song or a sound needs it. */
static dkr_sfx_bank_t sVoiceSfx[DKR_AFX_VOICE_PACK_COUNT];
static uint8_t sVoiceWanted[DKR_AFX_VOICE_PACK_COUNT]; /* Most important first. */
static uint8_t sVoiceWantedCount;
#define DKR_FALLBACK_SLOTS DKR_SFX_SLOTS
static struct {
    dkr_sfx_bank_t bank;
    uint16_t id;
    uint8_t loading, requested;
    int result;
} sFallback[DKR_FALLBACK_SLOTS];
static uint32_t sFallbackGeneration;
static uint16_t sSceneLevel = UINT16_MAX;
static uint8_t sVehicleMask;
static int sReady;
static volatile uint8_t sWantedSequence;
// While a scene loads, nothing new starts: sounds wait as if no voice were free
// and the song waits to be activated, so all of it begins with the scene's
// first frame instead of over the loading screen.
static volatile uint8_t sSceneHeld;
static semaphore_t sSfxLoadWake = SEM_INITIALIZER(0);
static volatile uint8_t sCurrentSequence;
static volatile uint8_t sFailedSequence;
static volatile uint8_t sGain = 255;
static volatile uint16_t sTempo = 120;
static uint16_t sAuthoredTempo = 120;
static volatile uint8_t sRoomReturns = 1;
static volatile uint32_t sControlDirty;
static volatile uint32_t sRoomDirty;
static volatile uint32_t sMuteDirty;
static volatile uint32_t sVolumeDirty;
/* Native-width atomics avoid SH4 GCC's returned halfword RMW register constraint. */
static volatile uint32_t sMuted;
static volatile uint8_t sVolume[16] = {
    127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127
};
static volatile uint8_t sFade[16] = {
    127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127
};

#if DKR_AICAFLOW_BENCHMARK
/* Follows one raw sound from trigger to completion: AICAFLOW_TRACE_SOUND=<id>. */
#ifndef DKR_AICAFLOW_TRACE_SOUND
#define DKR_AICAFLOW_TRACE_SOUND 0
#endif
static void benchmark_sound(unsigned id, const char *what, int a, int b, int c) {
    static int last[4];
    int now[4] = { what[0], a, b, c };
    if (id != DKR_AICAFLOW_TRACE_SOUND || !memcmp(last, now, sizeof(now))) return;
    memcpy(last, now, sizeof(now));
    printf("DKR AICAFLOW BENCH: sound %u %s %d %d %d\n", id, what, a, b, c);
    if (what[0] != 'p' || !a) return;
    /* A rejected trigger: show what holds the voices. */
    extern short gNumActiveSounds;
    printf("DKR AICAFLOW BENCH: %d active sounds, slots id/priority:", gNumActiveSounds);
    for (unsigned i = 0; i < DKR_SFX_SLOTS; ++i)
        if (sSfxSlots[i].instance)
            printf(" %u/%u%s", sSfxSlots[i].sound->id, sSfxSlots[i].priority,
                   !sSfxSlots[i].owner ? "(preempted)" : sSfxSlots[i].stopping ? "(stopping)" : "");
    printf("\n");
}
#endif

static afx_asset_t jingle_flow(uint8_t sequence) {
    for (uint32_t i = 0; i < DKR_AFX_JINGLE_COUNT; ++i) {
        if (dkr_afx_jingle_sequences[i] == sequence) return sJingleFlows[i];
    }
    return AFX_ASSET_INVALID;
}

static afx_asset_t music_flow_for(uint8_t sequence) {
    if (sSong.flow && sSong.sequence == sequence) return sSong.flow;
    return jingle_flow(sequence);
}

/* DKR owns this room recipe and can tune it in normal C code. The reduced wet
 * gain and feedback retain the N64-like space without masking the lead line. */
static int dsp_room_upload(void) {
    afx_dsp_program_t program;
    int result = afx_dsp_program_room(&program, 20480, 12288, false, 128, false);
    if (!result) result = afx_dsp_scene_program(&program, sizeof(program));
    return result;
}

int dkr_afx_sfx_stop_all(void) {
    uint32_t i, attempt;
    for (attempt = 0; attempt < 100; ++attempt) {
        int active = 0;
        if (afx_update()) { thd_sleep(1); continue; }
        update_sfx();
        for (i = 0; i < DKR_SFX_SLOTS; ++i) {
            if (sSfxSlots[i].instance) {
                active = 1;
                /* Activation may still be pending on the first attempt. */
                int result = afx_instance_stop(sSfxSlots[i].instance);
                (void)result;
            }
        }
        if (!active) return AFX_OK;
        thd_sleep(1);
    }
    return -AFX_BUSY;
}

static int read_asset(const char *path, uint8_t **out_data, uint32_t *out_size) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data = NULL;
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return -AFX_BAD_BOUNDS; }
    length = ftell(file);
    if (length <= 0 || length > AFX_ASSET_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file); return -AFX_BAD_BOUNDS;
    }
    data = memalign(32, (size_t)length);
    if (!data) { fclose(file); return -AFX_NO_HOST_RAM; }
    if (fread(data, 1, (size_t)length, file) != (size_t)length || ferror(file)) {
        free(data); fclose(file); return -AFX_BAD_BOUNDS;
    }
    fclose(file);
    *out_data = data;
    *out_size = (uint32_t)length;
    return AFX_OK;
}

static int sfx_flow_load(dkr_sfx_bank_t *bank, dkr_sfx_sound_t *sound,
                         uint16_t id, const char *path) {
    afx_file_header_t header;
    uint8_t *data;
    uint32_t bytes;
    int result = read_asset(path, &data, &bytes);
    if (result) return result;
    /* Only the header is needed here; the driver checks the flow on upload. */
    result = afx_file_validate_layout(data, bytes, &header);
    if (!result && (!header.setup_count || header.setup_count > UINT16_MAX)) result = -AFX_BAD_FORMAT;
    if (!result) {
        sound->fields = calloc(header.setup_count, AFX_SETUP_BYTES);
        if (!sound->fields) result = -AFX_NO_HOST_RAM;
    }
    if (!result) {
        const uint8_t *setups = data + header.image_offset;
        for (uint32_t setup = 0; setup < header.setup_count; ++setup)
            for (uint32_t field = 0; field < AFX_FIELD_COUNT; ++field)
                sound->fields[setup * AFX_FIELD_COUNT + field] =
                    afx_read16(setups + setup * AFX_SETUP_BYTES + field * 2);
        uint64_t started = timer_us_gettime64();
        result = DKR_AFX_RETRY(afx_bank_flow_upload(&bank->bank, data, bytes, &sound->flow));
        benchmark_flow_upload(started, bytes);
    }
    free(data);
    if (result) { free(sound->fields); *sound = (dkr_sfx_sound_t){0}; return result; }
    sound->id = id;
    sound->channels = (uint16_t)header.setup_count;
    return AFX_OK;
}

static int sfx_bank_load(dkr_sfx_bank_t *bank, const char *afb, const char *controls,
                         const uint16_t *ids, uint16_t count) {
    int result;
    if (!bank || bank->bank.asset || !count) return -AFX_BAD_COMMAND;
    uint64_t started = timer_us_gettime64();
    result = DKR_AFX_RETRY(afx_bank_load_file(&bank->bank, afb));
    benchmark_bank_load(started, result ? 0 : bank->bank.bytes);
    if (result) return result;
    bank->sounds = calloc(count, sizeof(*bank->sounds));
    if (!bank->sounds) { (void)DKR_AFX_RETRY(afx_bank_release(&bank->bank)); return -AFX_NO_HOST_RAM; }
    bank->sound_count = count;
    for (uint16_t i = 0; i < count; ++i) {
        char path[112];
        snprintf(path, sizeof(path), "%s/%u.afx", controls, ids[i]);
        result = sfx_flow_load(bank, &bank->sounds[i], ids[i], path);
        if (result) break;
    }
    if (!result) return AFX_OK;
    /* A pack that cannot be released stays described so a later release can retry. */
    if (sfx_bank_release(bank)) printf("DKR AICAFLOW: %s left in AICA RAM after a failed load\n", afb);
    return result;
}

static int sfx_bank_release(dkr_sfx_bank_t *bank) {
    int result = AFX_OK;
    if (!bank) return -AFX_BAD_COMMAND;
    for (uint16_t i = 0; i < bank->sound_count; ++i) {
        if (!bank->sounds[i].flow) continue;
        if ((result = DKR_AFX_RETRY(afx_asset_free(bank->sounds[i].flow)))) return result;
        bank->sounds[i].flow = AFX_ASSET_INVALID; /* A retry must not free it twice. */
    }
    if (bank->bank.asset && (result = DKR_AFX_RETRY(afx_bank_release(&bank->bank)))) return result;
    for (uint16_t i = 0; i < bank->sound_count; ++i) free(bank->sounds[i].fields);
    free(bank->sounds); *bank = (dkr_sfx_bank_t){0};
    return AFX_OK;
}

static const char *sfx_scene_path(uint16_t level, char path[64]) {
    if (level >= DKR_AFX_SCENE_COUNT || !dkr_afx_scene_has_sfx[level]) return NULL;
    snprintf(path, 64, DKR_SCENE_SFX_DIR "/%u.afb", level);
    return path;
}

static const char *sfx_vehicle_path(uint16_t level) {
    return level < DKR_AFX_SCENE_COUNT && dkr_afx_scene_vehicle_mask[level]
               ? DKR_VEHICLE_SFX_PATH : NULL;
}


/* Gives back the space of the least important voice pack that is not playing.
 * Returns 0 when there was none. The caller holds the game audio mutex. */
static int voices_release_one(void) {
    for (unsigned i = sVoiceWantedCount; i-- > 0;) {
        dkr_sfx_bank_t *bank = &sVoiceSfx[sVoiceWanted[i]];
        if (!bank->bank.asset || fallback_bank_active(bank)) continue;
        if (!sfx_bank_release(bank)) return 1;
    }
    return 0;
}

static int voices_release_all(void) {
    for (unsigned i = 0; i < DKR_AFX_VOICE_PACK_COUNT; ++i) {
        int result = sfx_bank_release(&sVoiceSfx[i]);
        if (result) return result;
    }
    return AFX_OK;
}

/* Loads the wanted packs in order of importance while each fits and still
 * leaves the reserve. A pack that does not fit is skipped, not an error: its
 * sounds then load one by one when they are first played. */
static void voices_load(void) {
    unsigned loaded = 0;
    for (unsigned i = 0; i < sVoiceWantedCount; ++i) {
        unsigned character = sVoiceWanted[i];
        afx_mem_stats_t memory;
        char afb[80], controls[80];
        int result;
        if (sVoiceSfx[character].bank.asset) { ++loaded; continue; }
        if (afx_mem_stats(&memory) ||
            memory.largest_free_block < dkr_afx_voice_bytes[character] + DKR_VOICE_RESERVE_BYTES) continue;
        snprintf(afb, sizeof(afb), DKR_VOICE_SFX_DIR "/%u.afb", character);
        snprintf(controls, sizeof(controls), DKR_VOICE_SFX_DIR "/%u", character);
        result = sfx_bank_load(&sVoiceSfx[character], afb, controls,
                               dkr_afx_voice_sound_ids + dkr_afx_voice_sound_first[character],
                               dkr_afx_voice_sound_count[character]);
        if (result) printf("DKR AICAFLOW: voice pack %u did not load (%d)\n", character, result);
        else ++loaded;
    }
    benchmark_memory("voice packs", loaded);
}

/* Makes the race voices of these racer characters resident, the first ones
 * first, as far as the level's own audio leaves room. Call after the level's
 * song is prepared, with the game audio mutex held. */
void dkr_afx_voices_prepare(const uint8_t *characters, unsigned count) {
    uint32_t wanted = 0;
    if (!sReady) return;
    sVoiceWantedCount = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned character = characters[i];
        if (character >= DKR_AFX_VOICE_PACK_COUNT || !dkr_afx_voice_sound_count[character] ||
            (wanted >> character & 1)) continue;
        wanted |= 1u << character;
        sVoiceWanted[sVoiceWantedCount++] = (uint8_t)character;
    }
    fallback_quiesce();
    for (unsigned i = 0; i < DKR_AFX_VOICE_PACK_COUNT; ++i)
        if (!(wanted >> i & 1) && !fallback_bank_active(&sVoiceSfx[i])) (void)sfx_bank_release(&sVoiceSfx[i]);
    voices_load();
}

static int scene_packs_load(uint16_t level, const char *vehicle_path, const char *scene_path) {
    int result = AFX_OK;
    if (vehicle_path && !sVehicleSfx.bank.asset)
        result = sfx_bank_load(&sVehicleSfx, vehicle_path,
                               DKR_ASSET_MOUNT "/build/dc/aicaflow/vehicle",
                               dkr_afx_vehicle_sound_ids, DKR_AFX_VEHICLE_SOUND_COUNT);
    if (!result && scene_path && !sSceneSfx.bank.asset) {
        char controls[64];
        snprintf(controls, sizeof(controls), DKR_SCENE_SFX_DIR "/%u", level);
        result = sfx_bank_load(&sSceneSfx, scene_path, controls,
                               dkr_afx_scene_sound_ids + dkr_afx_scene_sound_first[level],
                               dkr_afx_scene_sound_count[level]);
    }
    return result;
}

/* The build plans total space, not contiguous space. Music kept playing across
 * a scene change can split it, so empty the level region and reload the song
 * behind the packs. The song restarts; this is the only path that does so. */
static int scene_packs_load_compacted(uint16_t level, const char *vehicle_path, const char *scene_path) {
    uint8_t sequence = sSong.sequence;
    int wanted = __atomic_load_n(&sWantedSequence, __ATOMIC_ACQUIRE) == sequence;
    int result;

    printf("DKR AICAFLOW: reloading sequence %u to fit level %u\n", sequence, level);
    __atomic_store_n(&sWantedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
    if ((result = music_retire()) || (result = song_release())) return result;
    if (sVehicleSfx.bank.asset && (result = sfx_bank_release(&sVehicleSfx))) return result;
    result = scene_packs_load(level, vehicle_path, scene_path);
    if (song_load(sequence)) printf("DKR AICAFLOW: sequence %u did not fit after level %u\n", sequence, level);
    else if (wanted) __atomic_store_n(&sWantedSequence, sequence, __ATOMIC_RELEASE);
    return result;
}

/* Loads everything the level's audio plan lists and returns only when it is
 * resident. The caller holds the game audio mutex and calls
 * dkr_afx_music_prepare() next, so the song is the last allocation. */
int dkr_afx_sfx_scene_prepare(uint16_t level) {
    char scene_path[64];
    const char *path = sfx_scene_path(level, scene_path);
    const char *vehicle_path = sfx_vehicle_path(level);
    int result;

    if (!sReady) return -AFX_BUSY;
    if (sSceneLevel == level && sVehicleMask == !!vehicle_path) return AFX_OK;
    /* A level owns its loops. Never let an old engine or ambience survive into
     * a new scene, including sounds from the resident bank. */
    result = dkr_afx_sfx_stop_all();
    if (result) return result;
    result = fallback_clear();
    if (result) return result;
    fallback_quiesce();
    result = fallback_clear();
    if (result) return result;
    /* Voice packs sit behind the old song; the new level's racers reload theirs. */
    result = voices_release_all();
    if (result) return result;
    sVoiceWantedCount = 0;
    if (sSceneSfx.bank.asset) {
        result = sfx_bank_release(&sSceneSfx);
        if (result) return result;
        sSceneLevel = UINT16_MAX;
    }
    if (sVehicleSfx.bank.asset && !vehicle_path) {
        result = sfx_bank_release(&sVehicleSfx);
        if (result) return result;
    }
    /* An idle song would sit between this level's packs and its own music. */
    if (sSong.flow && !song_in_use()) {
        result = song_release();
        if (result) return result;
    }
    result = scene_packs_load(level, vehicle_path, path);
    if (result == -AFX_NO_AICA_RAM && sSong.flow)
        result = scene_packs_load_compacted(level, vehicle_path, path);
    sVehicleMask = sVehicleSfx.bank.asset != AFX_ASSET_INVALID;
    if (result) {
        printf("DKR AICAFLOW: level %u audio plan did not load (%d)\n", level, result);
        return result;
    }
    sSceneLevel = level;
    benchmark_memory("level", level);
    return AFX_OK;
}


int dkr_afx_init(void) {
    int result;
    uint64_t init_started = timer_us_gettime64();
    result = afx_init(firmware, sizeof(firmware));
    benchmark_init(init_started);
    /* Permanent assets load first and are never freed, so they stay contiguous
     * below the space the build plans for each level. */
    if (!result) result = sfx_bank_load(&sResidentSfx, DKR_RESIDENT_SFX_PATH,
                                        DKR_ASSET_MOUNT "/build/dc/aicaflow/core",
                                        dkr_afx_resident_sound_ids, DKR_AFX_RESIDENT_SOUND_COUNT);
    if (!result) {
        uint64_t started = timer_us_gettime64();
        result = afx_bank_load_file(&sJingleBank, DKR_JINGLE_BANK_PATH);
        benchmark_bank_load(started, result ? 0 : sJingleBank.bytes);
    }
    for (uint32_t i = 0; !result && i < DKR_AFX_JINGLE_COUNT; ++i)
        result = music_flow_load(&sJingleBank, DKR_JINGLE_CONTROL_DIR,
                                 dkr_afx_jingle_sequences[i], &sJingleFlows[i]);
    if (!result) result = dsp_room_upload();
    if (!result) {
        afx_mem_stats_t memory;
        if (!afx_mem_stats(&memory) && memory.largest_free_block < DKR_AFX_LEVEL_MAX_BYTES)
            printf("DKR AICAFLOW: %lu bytes free for levels, the build planned %lu\n",
                   (unsigned long)memory.largest_free_block, (unsigned long)DKR_AFX_LEVEL_MAX_BYTES);
    }
    if (!result && !music_flow_for(DKR_STARTUP_MUSIC)) result = song_load(DKR_STARTUP_MUSIC);
    if (!result) {
        kthread_attr_t attr = {
            /* File I/O and flow validation need the normal KOS thread stack. */
            .create_detached = 1, .stack_size = 0, .stack_ptr = NULL,
            .prio = 12, .label = "sfx-loader", .disable_tls = 1
        };
        if (!thd_create_ex(&attr, sfx_loader, NULL)) result = -AFX_NO_HOST_RAM;
    }
    sReady = result == 0;
    if (!result) benchmark_reset();
    if (result) printf("DKR AICAFLOW: init failed (%d)\n", result);
    return result;
}

static const dkr_sfx_bank_t *sfx_bank_for(uint16_t id, uint32_t *index) {
    const dkr_sfx_bank_t *banks[3 + DKR_AFX_VOICE_PACK_COUNT] = { &sSceneSfx, &sVehicleSfx, &sResidentSfx };
    uint32_t i;
    uint32_t bank;
    for (i = 0; i < DKR_AFX_VOICE_PACK_COUNT; ++i) banks[3 + i] = &sVoiceSfx[i];
    for (bank = 0; bank < sizeof(banks) / sizeof(banks[0]); ++bank) {
        for (i = 0; banks[bank]->bank.asset && i < banks[bank]->sound_count; ++i) {
            if (banks[bank]->sounds[i].id == id) {
                *index = i;
                return banks[bank];
            }
        }
    }
    for (i = 0; i < DKR_FALLBACK_SLOTS; ++i) {
        if (sFallback[i].id == id && !sFallback[i].loading &&
            !sFallback[i].requested && !sFallback[i].result &&
            sFallback[i].bank.bank.asset) {
            *index = 0;
            return &sFallback[i].bank;
        }
    }
    return NULL;
}

static int fallback_bank_active(const dkr_sfx_bank_t *bank) {
    for (unsigned i = 0; i < DKR_SFX_SLOTS; ++i)
        if (sSfxSlots[i].instance && sSfxSlots[i].bank == bank) return 1;
    return 0;
}

/* Called with the game audio mutex held. Loading slots belong to the worker. */
static int fallback_clear(void) {
    ++sFallbackGeneration;
    for (unsigned i = 0; i < DKR_FALLBACK_SLOTS; ++i) {
        if (sFallback[i].loading) continue;
        int result = sfx_bank_release(&sFallback[i].bank);
        if (result) return result;
        sFallback[i].id = sFallback[i].requested = 0;
        sFallback[i].result = 0;
    }
    return AFX_OK;
}

/* Waits until the fallback worker holds no load, so a planned load is the only
 * AICA allocation in progress. The caller holds the game audio mutex, which
 * also keeps the worker from starting another one. */
static void fallback_quiesce(void) {
    for (;;) {
        unsigned busy = 0;
        for (unsigned i = 0; i < DKR_FALLBACK_SLOTS; ++i)
            busy |= sFallback[i].loading | sFallback[i].requested;
        if (!busy) return;
        pc_audio_unlock();
        thd_sleep(1);
        pc_audio_lock();
    }
}

/* Releases cached fallback sounds that nothing is playing or waiting for. */
static void fallback_release_idle(unsigned keep) {
    for (unsigned j = 0; j < DKR_FALLBACK_SLOTS; ++j) {
        if (j == keep || sFallback[j].loading || sFallback[j].requested ||
            sndp_aicaflow_bank_pending(sFallback[j].id) ||
            fallback_bank_active(&sFallback[j].bank)) continue;
        if (!sfx_bank_release(&sFallback[j].bank)) sFallback[j].id = 0;
    }
}

static int fallback_request(uint16_t id) {
    if (!id || id > 784) return -AFX_BAD_COMMAND;
    for (unsigned i = 0; i < DKR_FALLBACK_SLOTS; ++i)
        if (sFallback[i].id == id) {
            int result = sFallback[i].result;
            /* Report this failure, but let a later trigger retry after voices retire. */
            if (result == -AFX_NO_AICA_RAM && !sfx_bank_release(&sFallback[i].bank))
                sFallback[i].id = 0;
            return result ? result : -AFX_BUSY;
        }
    /* Use vacant slots before discarding an idle cached sound. */
    for (unsigned evict = 0; evict < 2; ++evict) {
        for (unsigned i = 0; i < DKR_FALLBACK_SLOTS; ++i) {
            if (!evict && sFallback[i].id) continue;
            if (sFallback[i].loading || sFallback[i].requested ||
                sndp_aicaflow_bank_pending(sFallback[i].id) ||
                fallback_bank_active(&sFallback[i].bank)) continue;
            if (sfx_bank_release(&sFallback[i].bank)) continue;
            sFallback[i].id = id;
            sFallback[i].result = 0;
            sFallback[i].requested = 1;
            benchmark_unplanned(id, sSceneLevel);
            sem_signal(&sSfxLoadWake);
            return -AFX_BUSY;
        }
    }
    return -AFX_BUSY; /* Bank I/O pressure must not consume voice retries. */
}

/* Never hold the game mutex across /pc or DMA. */
static void fallback_load(void) {
    unsigned i;
    uint32_t generation;
    uint16_t id;
    char path[80];
    int result;
    pc_audio_lock();
    for (i = 0; i < DKR_FALLBACK_SLOTS; ++i)
        if (sFallback[i].requested) break;
    if (i == DKR_FALLBACK_SLOTS) { pc_audio_unlock(); return; }
    generation = sFallbackGeneration;
    id = sFallback[i].id;
    sFallback[i].requested = 0;
    sFallback[i].loading = 1;
    pc_audio_unlock();
    snprintf(path, sizeof(path), DKR_ASSET_MOUNT "/build/dc/aicaflow/fallback/%u.afb", id);
    {
        char controls[80];
        snprintf(controls, sizeof(controls), DKR_ASSET_MOUNT "/build/dc/aicaflow/fallback/controls");
        result = sfx_bank_load(&sFallback[i].bank, path, controls, &id, 1);
    }
    pc_audio_lock();
    if (result == -AFX_NO_AICA_RAM && generation == sFallbackGeneration) {
        /* Make room without touching the level's assets or playing sounds:
         * first the idle unplanned sounds, then the least important voices. */
        fallback_release_idle(i);
        do {
            char controls[80];
            pc_audio_unlock();
            snprintf(controls, sizeof(controls), DKR_ASSET_MOUNT "/build/dc/aicaflow/fallback/controls");
            result = sfx_bank_load(&sFallback[i].bank, path, controls, &id, 1);
            pc_audio_lock();
        } while (result == -AFX_NO_AICA_RAM && generation == sFallbackGeneration && voices_release_one());
    }
    if (generation != sFallbackGeneration) {
        int released = sfx_bank_release(&sFallback[i].bank);
        sFallback[i].id = 0;
        if (released) result = released;
    }
    sFallback[i].result = result;
    sFallback[i].loading = 0;
    pc_audio_unlock();
}

static int sfx_preempt(uint8_t priority) {
    dkr_sfx_slot_t *victim = NULL;
    uint32_t i;

    for (i = 0; i < DKR_SFX_SLOTS; ++i) {
        dkr_sfx_slot_t *slot = &sSfxSlots[i];
        if (!slot->owner || slot->priority > priority) continue;
        if (!victim || slot->priority < victim->priority) victim = slot;
    }
    if (!victim || afx_instance_stop(victim->instance)) return -AFX_NO_EXEC_BUDGET;
    {
        void *owner = victim->owner;
        victim->owner = NULL; /* Completion must not deallocate a reused state. */
        sndp_aicaflow_preempt(owner);
    }
    return -AFX_BUSY;
}

static int sfx_play(uint16_t id, void *owner, uint8_t priority);

int dkr_afx_sfx_play(uint16_t id, void *owner, uint8_t priority) {
    int result = sfx_play(id, owner, priority);
    benchmark_sound(id, "play result/priority", result, priority, 0);
    return result;
}

void dkr_afx_scene_hold(int held) {
    __atomic_store_n(&sSceneHeld, held ? 1 : 0, __ATOMIC_RELEASE);
}

static int sfx_play(uint16_t id, void *owner, uint8_t priority) {
    const dkr_sfx_bank_t *bank;
    uint32_t sound_index;
    uint32_t i;
    if (!sReady || !owner) return -AFX_BAD_COMMAND;
    if (__atomic_load_n(&sSceneHeld, __ATOMIC_ACQUIRE)) return -AFX_BUSY; /* retried once the scene starts */
    bank = sfx_bank_for(id, &sound_index);
    if (!bank) return fallback_request(id);
    if (!sndp_aicaflow_voice_available(owner)) {
        return sfx_preempt(priority);
    }
    for (i = 0; i < DKR_SFX_SLOTS; ++i) {
        dkr_sfx_slot_t *slot = &sSfxSlots[i];
        int result;
        if (slot->instance) continue;
        result = benchmark_activate(bank->sounds[sound_index].flow, &slot->instance, "sound", id);
        if (result == -AFX_NO_EXEC_BUDGET || result == -AFX_NO_CHANNELS) {
            return sfx_preempt(priority);
        }
        if (result) return result;
        slot->owner = owner;
        slot->bank = bank;
        slot->sound = &bank->sounds[sound_index];
        slot->dirty = DKR_SFX_DIRTY_VOLUME | DKR_SFX_DIRTY_PITCH |
                      DKR_SFX_DIRTY_PAN | DKR_SFX_DIRTY_FX;
        slot->volume = 255;
        slot->pitch = 1.0f;
        slot->pan = 64;
        slot->fx = 0;
        slot->priority = priority;
        slot->initializing = 1;
        return AFX_OK;
    }
    return sfx_preempt(priority);
}

static dkr_sfx_slot_t *sfx_slot(void *owner) {
    uint32_t i;
    for (i = 0; i < DKR_SFX_SLOTS; ++i) {
        if (sSfxSlots[i].owner == owner) return &sSfxSlots[i];
    }
    return NULL;
}

void dkr_afx_sfx_start_controls(void *owner, uint8_t gain, float pitch, uint8_t pan, uint8_t fx) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    slot->volume = gain;
    slot->pitch = pitch;
    slot->pan = pan;
    slot->fx = fx;
    benchmark_sound(slot->sound->id, "start gain/pan/fx", gain, pan, fx);
    /* Activation marked all fields dirty; publish the final initial values together. */
    apply_sfx_controls(slot);
}

void dkr_afx_sfx_stop(void *owner) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (slot) {
        slot->stopping = 1;
        int result = afx_instance_stop(slot->instance);
        (void)result;
    }
}

void dkr_afx_sfx_priority(void *owner, uint8_t priority) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (slot) slot->priority = priority;
}

void dkr_afx_sfx_volume(void *owner, uint8_t gain) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    if (slot->volume != gain) {
        slot->volume = gain;
        slot->dirty |= DKR_SFX_DIRTY_VOLUME;
    }
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_pitch(void *owner, float pitch) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    if (slot->pitch != pitch) {
        slot->pitch = pitch;
        slot->dirty |= DKR_SFX_DIRTY_PITCH;
    }
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_pan(void *owner, uint8_t pan) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    if (slot->pan != pan) {
        slot->pan = pan;
        slot->dirty |= DKR_SFX_DIRTY_PAN;
    }
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_fx(void *owner, uint8_t fx) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    if (slot->fx != fx) {
        slot->fx = fx;
        slot->dirty |= DKR_SFX_DIRTY_FX;
    }
    if (slot->initializing) apply_sfx_controls(slot);
}


void dkr_afx_music_play(uint8_t sequence) {
    if (sequence == DKR_SEQUENCE_NONE2) sequence = DKR_SEQUENCE_NONE;
    pc_audio_lock();
    if (!dkr_afx_music_prepare(sequence)) {
        __atomic_store_n(&sFailedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&sWantedSequence, sequence, __ATOMIC_RELEASE);
    }
    pc_audio_unlock();
}

void dkr_afx_music_stop(void) {
    __atomic_store_n(&sWantedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
}

int dkr_afx_music_playing(void) {
    return __atomic_load_n(&sCurrentSequence, __ATOMIC_ACQUIRE) != DKR_SEQUENCE_NONE;
}

uint8_t dkr_afx_music_current(void) {
    return __atomic_load_n(&sCurrentSequence, __ATOMIC_ACQUIRE);
}

void dkr_afx_music_gain(uint8_t gain) {
    if (__atomic_exchange_n(&sGain, gain, __ATOMIC_ACQ_REL) != gain)
        __atomic_fetch_or(&sControlDirty, 1u, __ATOMIC_RELEASE);
}

void dkr_afx_music_tempo(uint16_t bpm) {
    if (__atomic_exchange_n(&sTempo, bpm, __ATOMIC_ACQ_REL) != bpm)
        __atomic_fetch_or(&sControlDirty, 2u, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_mute(uint8_t lane, int muted) {
    uint32_t bit;
    if (lane >= 16) return;
    bit = 1u << lane;
    uint32_t previous = muted ? __atomic_fetch_or(&sMuted, bit, __ATOMIC_RELAXED)
                              : __atomic_fetch_and(&sMuted, ~bit, __ATOMIC_RELAXED);
    if (!!(previous & bit) != !!muted)
        __atomic_fetch_or(&sMuteDirty, bit, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_volume(uint8_t lane, uint8_t volume) {
    if (lane >= 16) return;
    if (__atomic_exchange_n(&sVolume[lane], volume, __ATOMIC_RELAXED) != volume)
        __atomic_fetch_or(&sVolumeDirty, 1u << lane, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_fade(uint8_t lane, uint8_t fade) {
    if (lane >= 16) return;
    if (__atomic_exchange_n(&sFade[lane], fade, __ATOMIC_RELAXED) != fade)
        __atomic_fetch_or(&sVolumeDirty, 1u << lane, __ATOMIC_RELEASE);
}

void dkr_afx_scene_reverb(uint8_t enabled) {
    if (__atomic_exchange_n(&sRoomReturns, enabled != 0, __ATOMIC_ACQ_REL) != (enabled != 0))
        __atomic_store_n(&sRoomDirty, 1, __ATOMIC_RELEASE);
}

static void apply_room_control(void) {
    if (__atomic_exchange_n(&sRoomDirty, 0, __ATOMIC_ACQ_REL) &&
        afx_dsp_scene_returns(__atomic_load_n(&sRoomReturns, __ATOMIC_ACQUIRE)) == -AFX_BUSY) {
        __atomic_store_n(&sRoomDirty, 1, __ATOMIC_RELEASE);
    }
}

static void apply_controls(void) {
    uint8_t values[32] = { 0 };
    uint32_t controls;
    uint32_t mask;
    int result;

    controls = __atomic_exchange_n(&sControlDirty, 0, __ATOMIC_ACQ_REL);
    if (controls & 1u) {
        result = afx_instance_gain(sMusic, __atomic_load_n(&sGain, __ATOMIC_ACQUIRE));
        if (result == -AFX_BUSY) __atomic_fetch_or(&sControlDirty, 1u, __ATOMIC_RELEASE);
    }
    if (controls & 2u) {
        uint32_t scale = (__atomic_load_n(&sTempo, __ATOMIC_ACQUIRE) * 256u +
                          sAuthoredTempo / 2u) / sAuthoredTempo;
        if (scale < 16) scale = 16;
        if (scale > 4096) scale = 4096;
        result = afx_instance_tempo(sMusic, (uint16_t)scale);
        if (result == -AFX_BUSY) __atomic_fetch_or(&sControlDirty, 2u, __ATOMIC_RELEASE);
    }

    mask = __atomic_exchange_n(&sMuteDirty, 0, __ATOMIC_ACQ_REL) & 0xffffu;
    if (mask) {
        uint16_t muted = __atomic_load_n(&sMuted, __ATOMIC_ACQUIRE);
        for (unsigned lane = 0; lane < 16; ++lane) values[lane] = (muted >> lane) & 1u;
        result = afx_instance_lanes_set(sMusic, AFX_LANE_MUTE, 0, mask, values);
        if (result == -AFX_BUSY) __atomic_fetch_or(&sMuteDirty, mask, __ATOMIC_RELEASE);
    }

    mask = __atomic_exchange_n(&sVolumeDirty, 0, __ATOMIC_ACQ_REL) & 0xffffu;
    if (mask) {
        for (unsigned lane = 0; lane < 16; ++lane) {
            unsigned volume = __atomic_load_n(&sVolume[lane], __ATOMIC_RELAXED);
            unsigned fade = __atomic_load_n(&sFade[lane], __ATOMIC_RELAXED);
            values[lane] = (uint8_t)((volume * fade * 255u + 8064u) / 16129u);
        }
        result = afx_instance_lanes_set(sMusic, AFX_LANE_GAIN, 0, mask, values);
        if (result == -AFX_BUSY) __atomic_fetch_or(&sVolumeDirty, mask, __ATOMIC_RELEASE);
    }
}


static int music_flow_load(const afx_bank_t *bank, const char *directory, uint8_t sequence,
                           afx_asset_t *out_flow) {
    char path[96];
    uint8_t *data = NULL;
    uint32_t bytes;
    int result;
    if (!bank->asset || !out_flow || !sequence || sequence >= DKR_SEQUENCE_COUNT)
        return -AFX_BAD_FORMAT;
    snprintf(path, sizeof(path), "%s/sequence_%u.afx", directory, sequence);
    result = read_asset(path, &data, &bytes);
    if (!result) {
        uint64_t started = timer_us_gettime64();
        result = DKR_AFX_RETRY(afx_bank_flow_upload(bank, data, bytes, out_flow));
        benchmark_flow_upload(started, bytes);
    }
    free(data);
    return result;
}

static int song_in_use(void) {
    return (sMusic && __atomic_load_n(&sCurrentSequence, __ATOMIC_ACQUIRE) == sSong.sequence) ||
           __atomic_load_n(&sWantedSequence, __ATOMIC_ACQUIRE) == sSong.sequence;
}

static int song_release(void) {
    int result;
    if (sSong.flow && (result = DKR_AFX_RETRY(afx_asset_free(sSong.flow)))) return result;
    sSong.flow = AFX_ASSET_INVALID;
    if (sSong.bank.asset && (result = DKR_AFX_RETRY(afx_bank_release(&sSong.bank)))) return result;
    sSong = (dkr_song_t){0};
    return AFX_OK;
}

static int song_load(uint8_t sequence) {
    char path[96];
    uint64_t started = timer_us_gettime64();
    int result;
    snprintf(path, sizeof(path), DKR_SONG_DIR "/sequence_%u.afb", sequence);
    result = DKR_AFX_RETRY(afx_bank_load_file(&sSong.bank, path));
    benchmark_bank_load(started, result ? 0 : sSong.bank.bytes);
    if (!result) result = music_flow_load(&sSong.bank, DKR_SONG_DIR, sequence, &sSong.flow);
    if (result) {
        (void)song_release();
        return result;
    }
    sSong.sequence = sequence;
    benchmark_memory("sequence", sequence);
    return AFX_OK;
}

/* Stops the song's instance and waits until the driver has recycled it. A
 * jingle keeps playing: it does not use the song's samples. */
static int music_retire(void) {
    if (!sMusic || __atomic_load_n(&sCurrentSequence, __ATOMIC_ACQUIRE) != sSong.sequence)
        return AFX_OK;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        afx_instance_status_t status;
        if (!afx_update() && !afx_instance_status(sMusic, &status)) {
            if (status.state != AFX_DONE && status.state != AFX_ERROR) {
                (void)afx_instance_stop(sMusic);
            } else if (!afx_instance_recycle(sMusic)) {
                sMusic = 0;
                __atomic_store_n(&sCurrentSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
                return AFX_OK;
            }
        }
        thd_sleep(1);
    }
    return -AFX_BUSY;
}

/* Only sounds missing from the level's plan load here, off the game thread. */
static void *sfx_loader(void *unused) {
    (void)unused;
    for (;;) {
        sem_wait(&sSfxLoadWake);
        fallback_load();
    }
    __builtin_unreachable();
}

/* Makes the song resident before returning. The caller holds DKR's audio
 * mutex. Replacing the song stops it first; jingles are always resident. */
int dkr_afx_music_prepare(uint8_t sequence) {
    int result;
    if (sequence == DKR_SEQUENCE_NONE2) return AFX_OK;
    if (!sReady || sequence == DKR_SEQUENCE_NONE) return sReady ? AFX_OK : -AFX_UNSUPPORTED;
    if (sequence >= DKR_SEQUENCE_COUNT) return -AFX_BAD_FORMAT;
    if (music_flow_for(sequence)) return AFX_OK;
    fallback_quiesce();
    if (sSong.flow && song_in_use())
        __atomic_store_n(&sWantedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
    result = music_retire();
    if (!result) result = song_release();
    if (!result) result = song_load(sequence);
    if (result == -AFX_NO_AICA_RAM) {
        /* Unplanned fallback sounds and voice packs may sit in the song's
         * space. The song comes first; the voices then take what is left. */
        fallback_release_idle(DKR_FALLBACK_SLOTS);
        do result = song_load(sequence);
        while (result == -AFX_NO_AICA_RAM && voices_release_one());
        if (sVoiceWantedCount) voices_load();
    }
    if (result) printf("DKR AICAFLOW: sequence %u did not load (%d)\n", sequence, result);
    return result;
}

/* Songs own their samples, so a speculative song would occupy the space the
 * build planned for the current level. Levels load their song synchronously. */
void dkr_afx_music_prefetch(uint8_t sequence) {
    (void)sequence;
}

static uint16_t scaled_pitch(uint16_t base, float scale) {
    int octave = (base >> 11) & 15;
    float ratio;
    int fraction;

    if (octave >= 8) octave -= 16;
    if (!(scale > 0.0f)) scale = 1.0f;
    ratio = (1.0f + (base & 1023) / 1024.0f) * scale;
    while (ratio >= 2.0f && octave < 7) ratio *= 0.5f, ++octave;
    while (ratio < 1.0f && octave > -8) ratio *= 2.0f, --octave;
    if (ratio < 1.0f) ratio = 1.0f;
    if (ratio >= 2.0f) ratio = 2047.0f / 1024.0f;
    fraction = (int)((ratio - 1.0f) * 1024.0f + 0.5f);
    if (fraction > 1023) fraction = 1023;
    return (uint16_t)(((octave & 15) << 11) | fraction);
}

static uint16_t panned_direct(uint16_t base, unsigned pan) {
    int steps = base & 0x10 ? -(base & 15) : base & 15;
    int delta = ((int)pan - 64) * 15;
    delta = delta >= 0 ? (delta + 31) / 63 : (delta - 31) / 63;
    steps += delta;
    if (steps < -15) steps = -15;
    if (steps > 15) steps = 15;
    return (uint16_t)((base & ~31u) | (steps >= 0 ? steps : (0x10 | -steps)));
}

static void apply_sfx_controls(dkr_sfx_slot_t *slot) {
    uint32_t dirty = slot->dirty;
    uint32_t channel;
    int result;
    if (!dirty) return;

    if (dirty & DKR_SFX_DIRTY_VOLUME) {
        result = afx_instance_gain(slot->instance, slot->volume);
        if (result) return;
    }
    for (channel = 0; channel < slot->sound->channels; ++channel) {
        const uint16_t *setup = slot->sound->fields + channel * AFX_FIELD_COUNT;
        uint16_t values[AFX_FIELD_COUNT];
        uint32_t mask = 0, count = 0;
        if (dirty & DKR_SFX_DIRTY_PITCH) {
            mask |= 1u << AFX_FIELD_PITCH;
            values[count++] = scaled_pitch(setup[AFX_FIELD_PITCH], slot->pitch);
        }
        if (dirty & DKR_SFX_DIRTY_FX) {
            unsigned fx = (slot->fx + ((setup[AFX_FIELD_DSP_SEND] >> 4) & 15u)) * 8u;
            if (fx > 127) fx = 127;
            mask |= 1u << AFX_FIELD_DSP_SEND;
            values[count++] = (uint16_t)((setup[AFX_FIELD_DSP_SEND] & ~0xf0u) |
                                         ((fx * 15u + 63u) / 127u) << 4);
        }
        if (dirty & DKR_SFX_DIRTY_PAN) {
            mask |= 1u << AFX_FIELD_DIRECT;
            values[count++] = panned_direct(setup[AFX_FIELD_DIRECT], slot->pan);
        }
        if (mask && afx_instance_patch(slot->instance, channel, mask, values)) return;
    }
    slot->dirty = 0;
}

static void update_sfx(void) {
    uint32_t i;
    for (i = 0; i < DKR_SFX_SLOTS; ++i) {
        dkr_sfx_slot_t *slot = &sSfxSlots[i];
        afx_instance_status_t status;
        void *owner;
        if (!slot->instance || afx_instance_status(slot->instance, &status)) continue;
        if (status.state == AFX_RUNNING || status.state == AFX_PARKED) {
            slot->initializing = 0;
            if (slot->stopping) (void)afx_instance_stop(slot->instance);
            else apply_sfx_controls(slot);
            continue;
        }
        if (status.state != AFX_DONE && status.state != AFX_ERROR) continue;
        if (afx_instance_recycle(slot->instance)) continue;
        benchmark_sound(slot->sound->id, "ended state/result/preempted", status.state, status.result, !slot->owner);
        benchmark_late("sound", slot->sound->id, &status);
        owner = slot->owner;
        *slot = (dkr_sfx_slot_t){0};
        sndp_aicaflow_complete(owner);
    }
}

void dkr_afx_update(void) {
    afx_instance_status_t status;
    uint8_t wanted;
#if DKR_AICAFLOW_BENCHMARK
    uint64_t started = timer_us_gettime64();
#endif

    if (!sReady || afx_update() < 0) goto done;
    apply_room_control();
    update_sfx();
    sndp_aicaflow_retry_pending();
    wanted = __atomic_load_n(&sWantedSequence, __ATOMIC_ACQUIRE);

    if (sMusic) {
        if (afx_instance_status(sMusic, &status)) goto done;
        if (status.state == AFX_RUNNING || status.state == AFX_PARKED) {
            if (wanted != sCurrentSequence) {
                /* Stop the whole flow before its asset is replaced: the driver
                 * releases every hardware channel reserved by this instance. */
                (void)afx_instance_stop(sMusic);
            } else {
                apply_controls();
            }
            goto done;
        }
        if (status.state != AFX_DONE && status.state != AFX_ERROR) goto done;
        if (afx_instance_recycle(sMusic)) goto done;
        benchmark_late("sequence", sCurrentSequence, &status);
        sMusic = 0;
        __atomic_store_n(&sCurrentSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
        goto done;
    }

    if (wanted != DKR_SEQUENCE_NONE && !__atomic_load_n(&sSceneHeld, __ATOMIC_ACQUIRE) &&
        wanted != __atomic_load_n(&sFailedSequence, __ATOMIC_ACQUIRE)) {
        afx_asset_t flow = music_flow_for(wanted);
        int result = flow ? AFX_OK : -AFX_BAD_FORMAT;
        if (!result) {
            sAuthoredTempo = __atomic_load_n(&sTempo, __ATOMIC_ACQUIRE);
            if (!sAuthoredTempo) sAuthoredTempo = 120;
            result = benchmark_activate(flow, &sMusic, "sequence", wanted);
        }
        if (result == -AFX_BUSY) goto done;
        if (result == -AFX_NO_EXEC_BUDGET || result == -AFX_NO_CHANNELS) {
            /* Music is continuous; make room before retrying next update. */
            (void)sfx_preempt(UINT8_MAX);
            goto done;
        }
        if (result) {
            __atomic_store_n(&sFailedSequence, wanted, __ATOMIC_RELEASE);
            printf("DKR AICAFLOW: sequence %u activation failed (%d)\n", wanted, result);
            goto done;
        }
        __atomic_store_n(&sCurrentSequence, wanted, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sControlDirty, 3u, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sMuteDirty, 0xffffu, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sVolumeDirty, 0xffffu, __ATOMIC_RELEASE);
    }
done:
#if DKR_AICAFLOW_BENCHMARK
    benchmark_update(started);
#endif
}
