#include "dkr_asset_mount.h"
#include "dkr_music_bench.h"
#include "audio_aicaflow.h"
#include "sound_ids.h"

#include <aicaflow/host.h>
#include <aicaflow/dsp.h>
#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include "sfx_manifest.h"
#include <kos.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DKR_SEQUENCE_NONE 0
#define DKR_SEQUENCE_NONE2 1
#define DKR_STARTUP_MUSIC_FIRST 35
#define DKR_STARTUP_MUSIC_SECOND 7
#define DKR_SFX_SLOTS 16
#define DKR_SFX_DIRTY_VOLUME 1
#define DKR_SFX_DIRTY_PITCH 2
#define DKR_SFX_DIRTY_PAN 4
#define DKR_SFX_DIRTY_FX 8

alignas(32) static const unsigned char firmware[] = {
#embed "../third_party/aicaflow/driver/arm7/aicaflow.drv"
};

#define DKR_RESIDENT_SFX_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/core.afb"
#define DKR_SCENE_SFX_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/scenes"
#define DKR_VEHICLE_SFX_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/vehicle.afb"
#define DKR_MUSIC_BANK_PATH DKR_ASSET_MOUNT "/build/dc/aicaflow/music.afb"
#define DKR_MUSIC_CONTROL_DIR DKR_ASSET_MOUNT "/build/dc/aicaflow/music_controls"
#define DKR_SEQUENCE_COUNT 66
#define DKR_SHORT_MUSIC_COUNT 25

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
static void *music_loader(void *unused);
static void *sfx_loader(void *unused);
static int music_flow_load(uint8_t sequence, afx_asset_t *out_flow);
static void collect_loaded_music(void);
static int fallback_request(uint16_t id);
static void fallback_load(void);
static int fallback_clear(void);
extern void pc_audio_lock(void);
extern void pc_audio_unlock(void);

static const uint8_t sShortMusicSequences[DKR_SHORT_MUSIC_COUNT] = {
    2, 3, 4, 5, 17, 18, 19, 25, 27, 30, 41, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 54, 56, 61, 62
};

static afx_asset_t sMusicFlow;
static afx_asset_t sPrefetchedMusicFlow;
static afx_asset_t sShortMusicFlows[DKR_SHORT_MUSIC_COUNT];
static afx_instance_t sMusic;
static uint8_t sMusicFlowSequence;
static uint8_t sPrefetchedMusicSequence;
static dkr_sfx_slot_t sSfxSlots[DKR_SFX_SLOTS];
static dkr_sfx_bank_t sResidentSfx;
static dkr_sfx_bank_t sSceneSfx;
static dkr_sfx_bank_t sVehicleSfx;
static afx_bank_t sMusicBank;
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
static volatile uint8_t sPreparedSequence;
static volatile uint8_t sPrefetchSequence;
static volatile uint8_t sLoadPreparedSequence;
static volatile uint8_t sLoadPrefetchSequence;
static volatile uint8_t sLoadingSequence;
static volatile uint8_t sLoadedSequence;
static volatile afx_asset_t sLoadedMusicFlow;
static volatile int sLoadedMusicResult;
static volatile uint8_t sLoadedMusicReady;
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
static volatile uint16_t sMuted;
static volatile uint8_t sVolume[16] = {
    127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127
};
static volatile uint8_t sFade[16] = {
    127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127
};

static afx_asset_t short_music_flow(uint8_t sequence) {
    for (uint32_t i = 0; i < DKR_SHORT_MUSIC_COUNT; ++i) {
        if (sShortMusicSequences[i] == sequence) return sShortMusicFlows[i];
    }
    return AFX_ASSET_INVALID;
}

static afx_asset_t music_flow_for(uint8_t sequence) {
    if (sMusicFlow && sMusicFlowSequence == sequence) return sMusicFlow;
    if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == sequence) return sPrefetchedMusicFlow;
    return short_music_flow(sequence);
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
        if (afx_update()) return -AFX_BUSY;
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
    if (length <= 0 || length > AFX_ASSET_LIMIT || fseek(file, 0, SEEK_SET)) {
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
    result = afx_file_validate(data, bytes, &header);
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
        result = afx_bank_flow_upload(&bank->bank, data, bytes, &sound->flow);
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
    result = afx_bank_load_file(&bank->bank, afb);
    if (result) return result;
    bank->sounds = calloc(count, sizeof(*bank->sounds));
    if (!bank->sounds) { (void)afx_bank_release(&bank->bank); return -AFX_NO_HOST_RAM; }
    bank->sound_count = count;
    for (uint16_t i = 0; i < count; ++i) {
        char path[112];
        snprintf(path, sizeof(path), "%s/%u.afx", controls, ids[i]);
        result = sfx_flow_load(bank, &bank->sounds[i], ids[i], path);
        if (result) break;
    }
    if (!result) return AFX_OK;
    for (uint16_t i = 0; i < count; ++i) {
        if (bank->sounds[i].flow) (void)afx_asset_free(bank->sounds[i].flow);
        free(bank->sounds[i].fields);
    }
    free(bank->sounds); (void)afx_bank_release(&bank->bank); *bank = (dkr_sfx_bank_t){0};
    return result;
}

static int sfx_bank_release(dkr_sfx_bank_t *bank) {
    int result = AFX_OK;
    if (!bank) return -AFX_BAD_COMMAND;
    for (uint16_t i = 0; i < bank->sound_count; ++i) {
        if (bank->sounds[i].flow && (result = afx_asset_free(bank->sounds[i].flow))) return result;
    }
    if (bank->bank.asset && (result = afx_bank_release(&bank->bank))) return result;
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
    if (sSceneSfx.bank.asset) {
        result = sfx_bank_release(&sSceneSfx);
        if (result) return result;
        sSceneLevel = UINT16_MAX;
    }
    if (sVehicleSfx.bank.asset && !vehicle_path) {
        result = sfx_bank_release(&sVehicleSfx);
        if (result) return result;
        sVehicleMask = 0;
    }
    result = AFX_OK;
    if (vehicle_path && !sVehicleSfx.bank.asset)
        result = sfx_bank_load(&sVehicleSfx, vehicle_path,
                               DKR_ASSET_MOUNT "/build/dc/aicaflow/vehicle",
                               dkr_afx_vehicle_sound_ids, DKR_AFX_VEHICLE_SOUND_COUNT);
    if (result) return result;
    if (vehicle_path && !sVehicleMask) {
        sVehicleMask = 1;
    }
    if (path) {
        afx_mem_stats_t memory;
        result = afx_mem_stats(&memory);
        if (result) return result;
        /* Keep enough room for the largest standalone SFX (52 KiB in this bank). */
        if (dkr_afx_scene_bytes[level] + 65536u > memory.largest_free_block) {
            path = NULL;
        }
    }
    if (path) {
        char controls[64];
        snprintf(controls, sizeof(controls), DKR_SCENE_SFX_DIR "/%u", level);
        result = sfx_bank_load(&sSceneSfx, path, controls,
                               dkr_afx_scene_sound_ids + dkr_afx_scene_sound_first[level],
                               dkr_afx_scene_sound_count[level]);
    } else result = AFX_OK;
    /* Music loading can consume the measured space before this allocation. */
    if (result == -AFX_NO_AICA_RAM) {
        path = NULL;
        result = AFX_OK;
    }
    if (!result) sSceneLevel = level;
    return result;
}

int dkr_afx_init(void) {
    int result;
    result = afx_init(firmware, sizeof(firmware));
    if (!result) result = sfx_bank_load(&sResidentSfx, DKR_RESIDENT_SFX_PATH,
                                        DKR_ASSET_MOUNT "/build/dc/aicaflow/core",
                                        dkr_afx_resident_sound_ids, DKR_AFX_RESIDENT_SOUND_COUNT);
    if (!result) result = afx_bank_load_file(&sMusicBank, DKR_MUSIC_BANK_PATH);
    for (uint32_t i = 0; !result && i < DKR_SHORT_MUSIC_COUNT; ++i)
        result = music_flow_load(sShortMusicSequences[i], &sShortMusicFlows[i]);
    if (!result) result = dsp_room_upload();
    if (!result) {
        result = music_flow_load(DKR_STARTUP_MUSIC_FIRST, &sMusicFlow);
        if (!result) sMusicFlowSequence = DKR_STARTUP_MUSIC_FIRST;
        if (!result) result = music_flow_load(DKR_STARTUP_MUSIC_SECOND, &sPrefetchedMusicFlow);
        if (!result) sPrefetchedMusicSequence = DKR_STARTUP_MUSIC_SECOND;
        if (result && sMusicFlow) {
            (void)afx_asset_free(sMusicFlow);
            sMusicFlow = AFX_ASSET_INVALID;
            sMusicFlowSequence = DKR_SEQUENCE_NONE;
        }
    }
    if (!result) {
        kthread_attr_t attr = {
            /* File I/O and flow validation need the normal KOS thread stack. */
            .create_detached = 1, .stack_size = 0, .stack_ptr = NULL,
            .prio = 10, .label = "aica-loader", .disable_tls = 1
        };
        if (!thd_create_ex(&attr, music_loader, NULL)) result = -AFX_NO_HOST_RAM;
        if (!result) {
            attr.label = "sfx-loader";
            attr.prio = 12;
            if (!thd_create_ex(&attr, sfx_loader, NULL)) result = -AFX_NO_HOST_RAM;
        }
    }
    sReady = result == 0;
    if (sReady && DKR_MUSIC_BENCH_SOLO_MASK) {
        __atomic_store_n(&sMuted, (uint16_t)~DKR_MUSIC_BENCH_SOLO_MASK, __ATOMIC_RELEASE);
        __atomic_store_n(&sMuteDirty, 0xffffu, __ATOMIC_RELEASE);
    }
    /* The capture flow is already preloaded below; avoid taking the game audio
       mutex while audio startup is still wiring its worker threads. */
    if (sReady && DKR_MUSIC_BENCH_SEQUENCE)
        __atomic_store_n(&sWantedSequence, DKR_MUSIC_BENCH_SEQUENCE, __ATOMIC_RELEASE);
    if (result) printf("DKR AICAFLOW: init failed (%d)\n", result);
    return result;
}

static const dkr_sfx_bank_t *sfx_bank_for(uint16_t id, uint32_t *index) {
    const dkr_sfx_bank_t *banks[] = { &sSceneSfx, &sVehicleSfx, &sResidentSfx };
    uint32_t i;
    uint32_t bank;
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
        /* Make room without touching resident music or playing sounds. */
        for (unsigned j = 0; j < DKR_FALLBACK_SLOTS; ++j) {
            if (j == i || sFallback[j].loading || sFallback[j].requested ||
                sndp_aicaflow_bank_pending(sFallback[j].id) ||
                fallback_bank_active(&sFallback[j].bank)) continue;
            if (!sfx_bank_release(&sFallback[j].bank)) sFallback[j].id = 0;
        }
        /* A speculative next song yields to a sound needed in this scene. */
        if (sPrefetchedMusicFlow &&
            sPrefetchedMusicSequence != __atomic_load_n(&sPreparedSequence, __ATOMIC_ACQUIRE) &&
            !afx_asset_free(sPrefetchedMusicFlow)) {
            sPrefetchedMusicFlow = AFX_ASSET_INVALID;
            sPrefetchedMusicSequence = DKR_SEQUENCE_NONE;
        }
        pc_audio_unlock();
        {
            char controls[80];
            snprintf(controls, sizeof(controls), DKR_ASSET_MOUNT "/build/dc/aicaflow/fallback/controls");
            result = sfx_bank_load(&sFallback[i].bank, path, controls, &id, 1);
        }
        pc_audio_lock();
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

int dkr_afx_sfx_play(uint16_t id, void *owner, uint8_t priority) {
    if (DKR_MUSIC_BENCH_SEQUENCE) {
        (void)id;
        (void)owner;
        (void)priority;
        return AFX_OK;
    }
    const dkr_sfx_bank_t *bank;
    uint32_t sound_index;
    uint32_t i;
    if (!sReady || !owner) return -AFX_BAD_COMMAND;
    bank = sfx_bank_for(id, &sound_index);
    if (!bank) return fallback_request(id);
    if (!sndp_aicaflow_voice_available(owner)) {
        return sfx_preempt(priority);
    }
    for (i = 0; i < DKR_SFX_SLOTS; ++i) {
        dkr_sfx_slot_t *slot = &sSfxSlots[i];
        int result;
        if (slot->instance) continue;
        result = afx_instance_activate(bank->sounds[sound_index].flow, &slot->instance);
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
    slot->volume = gain;
    slot->dirty |= DKR_SFX_DIRTY_VOLUME;
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_pitch(void *owner, float pitch) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    slot->pitch = pitch;
    slot->dirty |= DKR_SFX_DIRTY_PITCH;
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_pan(void *owner, uint8_t pan) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    slot->pan = pan;
    slot->dirty |= DKR_SFX_DIRTY_PAN;
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_sfx_fx(void *owner, uint8_t fx) {
    dkr_sfx_slot_t *slot = sfx_slot(owner);
    if (!slot) return;
    slot->fx = fx;
    slot->dirty |= DKR_SFX_DIRTY_FX;
    if (slot->initializing) apply_sfx_controls(slot);
}

void dkr_afx_music_play(uint8_t sequence) {
    if (DKR_MUSIC_BENCH_SEQUENCE) sequence = DKR_MUSIC_BENCH_SEQUENCE;
    if (sequence == DKR_SEQUENCE_NONE2) sequence = DKR_SEQUENCE_NONE;
    pc_audio_lock();
    int result = sequence ? dkr_afx_music_prepare(sequence) : AFX_OK;
    while (!result && sequence && !music_flow_for(sequence)) {
        collect_loaded_music();
        if (__atomic_load_n(&sFailedSequence, __ATOMIC_ACQUIRE) == sequence) {
            result = -AFX_BAD_FORMAT;
            break;
        }
        pc_audio_unlock();
        thd_sleep(1);
        pc_audio_lock();
    }
    if (!result) {
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
    __atomic_store_n(&sGain, gain, __ATOMIC_RELEASE);
    __atomic_fetch_or(&sControlDirty, 1u, __ATOMIC_RELEASE);
}

void dkr_afx_music_tempo(uint16_t bpm) {
    __atomic_store_n(&sTempo, bpm, __ATOMIC_RELEASE);
    __atomic_fetch_or(&sControlDirty, 2u, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_mute(uint8_t lane, int muted) {
    uint16_t bit;
    if (lane >= 16) return;
    bit = (uint16_t)(1u << lane);
    if (muted) __atomic_fetch_or(&sMuted, bit, __ATOMIC_RELAXED);
    else __atomic_fetch_and(&sMuted, (uint16_t)~bit, __ATOMIC_RELAXED);
    __atomic_fetch_or(&sMuteDirty, bit, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_volume(uint8_t lane, uint8_t volume) {
    if (lane >= 16) return;
    __atomic_store_n(&sVolume[lane], volume, __ATOMIC_RELAXED);
    __atomic_fetch_or(&sVolumeDirty, 1u << lane, __ATOMIC_RELEASE);
}

void dkr_afx_music_lane_fade(uint8_t lane, uint8_t fade) {
    if (lane >= 16) return;
    __atomic_store_n(&sFade[lane], fade, __ATOMIC_RELAXED);
    __atomic_fetch_or(&sVolumeDirty, 1u << lane, __ATOMIC_RELEASE);
}

void dkr_afx_scene_reverb(uint8_t enabled) {
    __atomic_store_n(&sRoomReturns, enabled != 0, __ATOMIC_RELEASE);
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

static int music_flow_load(uint8_t sequence, afx_asset_t *out_flow) {
    char path[80];
    uint8_t *data = NULL;
    uint32_t bytes;
    int result;
    if (!sMusicBank.asset || !out_flow || !sequence || sequence >= DKR_SEQUENCE_COUNT)
        return -AFX_BAD_FORMAT;
    snprintf(path, sizeof(path), DKR_MUSIC_CONTROL_DIR "/sequence_%u.afx", sequence);
    result = read_asset(path, &data, &bytes);
    if (!result) result = afx_bank_flow_upload(&sMusicBank, data, bytes, out_flow);
    free(data);
    return result;
}

static void music_load_enqueue(uint8_t sequence, int prepared) {
    volatile uint8_t *queue = prepared ? &sLoadPreparedSequence : &sLoadPrefetchSequence;

    if (__atomic_load_n(&sLoadingSequence, __ATOMIC_ACQUIRE) == sequence ||
        (__atomic_load_n(&sLoadedMusicReady, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&sLoadedSequence, __ATOMIC_ACQUIRE) == sequence) ||
        __atomic_load_n(queue, __ATOMIC_ACQUIRE) == sequence) return;
    __atomic_store_n(queue, sequence, __ATOMIC_RELEASE);
}

/* Music callers may wait with the game audio mutex held. Keep SFX I/O on
 * its own worker so its publication lock cannot block those music requests. */
static void *sfx_loader(void *unused) {
    (void)unused;
    for (;;) {
        fallback_load();
        thd_sleep(1);
    }
    __builtin_unreachable();
}

static void *music_loader(void *unused) {
    (void)unused;
    for (;;) {
        afx_asset_t flow = AFX_ASSET_INVALID;
        uint8_t sequence;
        int result;

        if (__atomic_load_n(&sPreparedSequence, __ATOMIC_ACQUIRE))
            sequence = __atomic_exchange_n(&sLoadPreparedSequence, DKR_SEQUENCE_NONE, __ATOMIC_ACQ_REL);
        else
            sequence = __atomic_exchange_n(&sLoadPrefetchSequence, DKR_SEQUENCE_NONE, __ATOMIC_ACQ_REL);
        if (!sequence) {
            thd_sleep(1);
            continue;
        }
        __atomic_store_n(&sLoadingSequence, sequence, __ATOMIC_RELEASE);
        result = music_flow_load(sequence, &flow);
        __atomic_store_n(&sLoadingSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
        while (__atomic_load_n(&sLoadedMusicReady, __ATOMIC_ACQUIRE)) thd_sleep(1);
        __atomic_store_n(&sLoadedMusicFlow, flow, __ATOMIC_RELAXED);
        __atomic_store_n(&sLoadedMusicResult, result, __ATOMIC_RELAXED);
        __atomic_store_n(&sLoadedSequence, sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&sLoadedMusicReady, 1, __ATOMIC_RELEASE);
    }
    __builtin_unreachable();
}

/* The caller holds DKR's audio mutex. A low-priority loader owns all file I/O;
 * dkr_afx_update only consumes completed flows and changes playback state. */
int dkr_afx_music_prepare(uint8_t sequence) {
    if (sequence == DKR_SEQUENCE_NONE2) return AFX_OK;
    if (!sReady || sequence == DKR_SEQUENCE_NONE) return sReady ? AFX_OK : -AFX_UNSUPPORTED;
    if (sequence >= DKR_SEQUENCE_COUNT) return -AFX_BAD_FORMAT;
    if ((sMusicFlow && sMusicFlowSequence == sequence) || short_music_flow(sequence)) {
        __atomic_store_n(&sPreparedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
        return AFX_OK;
    }
    __atomic_store_n(&sFailedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&sPreparedSequence, sequence, __ATOMIC_RELEASE);
    if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == sequence) return AFX_OK;
    music_load_enqueue(sequence, 1);
    return AFX_OK;
}

void dkr_afx_music_prefetch(uint8_t sequence) {
    if (!sReady || sequence <= DKR_SEQUENCE_NONE2 || sequence >= DKR_SEQUENCE_COUNT ||
        music_flow_for(sequence)) return;
    __atomic_store_n(&sPrefetchSequence, sequence, __ATOMIC_RELEASE);
    music_load_enqueue(sequence, 0);
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
        owner = slot->owner;
        *slot = (dkr_sfx_slot_t){0};
        sndp_aicaflow_complete(owner);
    }
}

static void collect_loaded_music(void) {
    afx_asset_t flow;
    uint8_t sequence;
    uint8_t prepared;
    uint8_t prefetch;
    int result;

    if (!__atomic_load_n(&sLoadedMusicReady, __ATOMIC_ACQUIRE)) return;
    flow = __atomic_load_n(&sLoadedMusicFlow, __ATOMIC_RELAXED);
    result = __atomic_load_n(&sLoadedMusicResult, __ATOMIC_RELAXED);
    sequence = __atomic_load_n(&sLoadedSequence, __ATOMIC_RELAXED);
    prepared = __atomic_load_n(&sPreparedSequence, __ATOMIC_ACQUIRE);
    prefetch = __atomic_load_n(&sPrefetchSequence, __ATOMIC_ACQUIRE);
    if (result == -AFX_NO_AICA_RAM && sequence == prepared &&
        sMusicFlow && sMusicFlowSequence != sequence) {
        /* A stopped song must not prevent its replacement from fitting. */
        if (sMusic || afx_asset_free(sMusicFlow)) return;
        sMusicFlow = AFX_ASSET_INVALID;
        sMusicFlowSequence = DKR_SEQUENCE_NONE;
        __atomic_store_n(&sLoadedMusicReady, 0, __ATOMIC_RELEASE);
        music_load_enqueue(sequence, 1);
        return;
    }
    if (!result && (sequence == prepared || sequence == prefetch)) {
        /* Promote the requested song before a later prefetch replaces it. */
        if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == prepared &&
            sequence != prepared) return;
        if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == sequence) {
            if (flow && afx_asset_free(flow)) return;
            flow = AFX_ASSET_INVALID;
        }
        if (sPrefetchedMusicFlow && sPrefetchedMusicSequence != sequence &&
            afx_asset_free(sPrefetchedMusicFlow)) return;
        if (sPrefetchedMusicFlow && sPrefetchedMusicSequence != sequence) {
            sPrefetchedMusicFlow = AFX_ASSET_INVALID;
            sPrefetchedMusicSequence = DKR_SEQUENCE_NONE;
        }
        if (!sPrefetchedMusicFlow) {
            sPrefetchedMusicFlow = flow;
            sPrefetchedMusicSequence = sequence;
            flow = AFX_ASSET_INVALID;
        }
        if (sequence == prefetch)
            __atomic_store_n(&sPrefetchSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
    } else if (flow && afx_asset_free(flow)) {
        return;
    }
    if (result && sequence == prepared) {
        __atomic_store_n(&sFailedSequence, sequence, __ATOMIC_RELEASE);
        __atomic_store_n(&sPreparedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&sLoadedMusicReady, 0, __ATOMIC_RELEASE);
}

void dkr_afx_update(void) {
    afx_instance_status_t status;
    uint8_t wanted;

    if (!sReady || afx_update() < 0) return;
    apply_room_control();
    update_sfx();
    sndp_aicaflow_retry_pending();
    collect_loaded_music();
    wanted = __atomic_load_n(&sWantedSequence, __ATOMIC_ACQUIRE);

    if (sMusic) {
        if (afx_instance_status(sMusic, &status)) return;
        if (status.state == AFX_RUNNING || status.state == AFX_PARKED) {
            if (wanted != sCurrentSequence) {
                /* Stop the whole flow before its asset is replaced: the driver
                 * releases every hardware channel reserved by this instance. */
                (void)afx_instance_stop(sMusic);
            } else {
                apply_controls();
            }
            return;
        }
        if (status.state != AFX_DONE && status.state != AFX_ERROR) return;
        if (afx_instance_recycle(sMusic)) return;
        sMusic = 0;
        __atomic_store_n(&sCurrentSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
        return;
    }

    {
        uint8_t prepared = __atomic_load_n(&sPreparedSequence, __ATOMIC_ACQUIRE);
        if (prepared) {
            int result;

            if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == prepared) {
                if (sMusicFlow && (result = afx_asset_free(sMusicFlow))) {
                    __atomic_store_n(&sFailedSequence, prepared, __ATOMIC_RELEASE);
                    return;
                }
                sMusicFlow = sPrefetchedMusicFlow;
                sMusicFlowSequence = sPrefetchedMusicSequence;
                sPrefetchedMusicFlow = AFX_ASSET_INVALID;
                sPrefetchedMusicSequence = DKR_SEQUENCE_NONE;
                __atomic_store_n(&sPreparedSequence, DKR_SEQUENCE_NONE, __ATOMIC_RELEASE);
                return;
            }
            return;
        }
    }

    if (wanted != DKR_SEQUENCE_NONE &&
        wanted != __atomic_load_n(&sFailedSequence, __ATOMIC_ACQUIRE)) {
        afx_asset_t flow = music_flow_for(wanted);
        int result = flow ? AFX_OK : -AFX_BAD_FORMAT;
        if (!result) {
            sAuthoredTempo = __atomic_load_n(&sTempo, __ATOMIC_ACQUIRE);
            if (!sAuthoredTempo) sAuthoredTempo = 120;
            result = afx_instance_activate(flow, &sMusic);
        }
        if (result == -AFX_BUSY) return;
        if (result == -AFX_NO_EXEC_BUDGET || result == -AFX_NO_CHANNELS) {
            /* Music is continuous; make room before retrying next update. */
            (void)sfx_preempt(UINT8_MAX);
            return;
        }
        if (result) {
            __atomic_store_n(&sFailedSequence, wanted, __ATOMIC_RELEASE);
            printf("DKR AICAFLOW: sequence %u activation failed (%d)\n", wanted, result);
            return;
        }
        __atomic_store_n(&sCurrentSequence, wanted, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sControlDirty, 3u, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sMuteDirty, 0xffffu, __ATOMIC_RELEASE);
        __atomic_fetch_or(&sVolumeDirty, 0xffffu, __ATOMIC_RELEASE);
    }
}
