#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include <sh4zam/shz_sh4zam.h>

#include "input.h"
#include "save.h"

typedef signed char s8;
typedef signed int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

// ---------------------------------------------------------------------------
// RSP microcode blobs (task-submission code references these; replaced by the
// HLE renderer / audio interpreter later)
// ---------------------------------------------------------------------------
u64 rspbootTextStart[1] = { 0 };
u64 rspbootTextEnd[1] = { 0 };
u64 aspMainTextStart[1] = { 0 };
u64 aspMainDataStart[1] = { 0 };
u64 rspF3DDKRXbusStart[1] = { 0 };
u64 rspF3DDKRDataXbusStart[1] = { 0 };
u64 rspF3DDKRFifoStart[1] = { 0 };
u64 rspF3DDKRDataFifoStart[1] = { 0 };

// ---------------------------------------------------------------------------
// Linker-script symbols (placed by the N64 linker script; static here)
// ---------------------------------------------------------------------------
// Becomes the real asset LUT/data location once the PC asset loader exists.
u8 __ASSETS_LUT_START[1] = { 0 };
u8 __ASSETS_LUT_END[1] = { 0 };
u8 *__ROM_END; // cheat-menu checksum bound
u8 *main_BSS_START[1] = { 0 };

// N64: the main heap is [end of BSS .. RAM_END], carved out by the linker.
// PC: a static pool. memory.c's "ramEnd - (s32)&gMainMemoryPool" sizing math
// still needs TARGET_PC surgery to use this pool's real size instead.
// Storage lives in memory.c, which has MemoryPoolSlot in scope; its size
// comes from -DPC_MAIN_POOL_BYTES in the platform makefile.

// ---------------------------------------------------------------------------
// Asset "DMA" — the DKR equivalent of the OoT port's DmaMgr_DmaRomToRam.
// On N64 the asset LUT and asset data sit in cart ROM right after the code,
// bracketed by __ASSETS_LUT_START/__ASSETS_LUT_END. On PC the same bytes live
// in the files the N64 build already produces (assets/assets.lut.bin and
// assets/assets.bin); dmacopy addresses are translated back to file offsets
// relative to the two stub symbols above.
// The LUT is an array of big-endian u32s — byteswapped once at load. Asset
// *contents* are left big-endian; each parse site gets fixed as it comes up.
// ---------------------------------------------------------------------------

#ifdef TARGET_DC
#define ASSET_DIR "/pc/assets/"
#else
#define ASSET_DIR "assets/"
#endif

static u8 *sAssetLut = NULL;
static u32 sAssetLutSize = 0;
static u8 *sAssetsBin = NULL;
static u32 sAssetsBinSize = 0;

static u8 *pc_load_file(const char *path, u32 *sizeOut) {
    FILE *f = fopen(path, "rb");
    long size;
    u8 *buf;

    if (f == NULL) {
        fprintf(stderr, "ASSETS: cannot open %s (run from the repo root, and build the N64 assets first)\n", path);
        exit(1);
    }
    // Make file reading faster
    setvbuf(f, NULL, _IONBF, 0);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t) size) {
        fprintf(stderr, "ASSETS: short read on %s\n", path);
        exit(1);
    }
    fclose(f);
    *sizeOut = (u32) size;
    return buf;
}

static void pc_assets_init(void) {
    u32 i;

    if (sAssetLut != NULL) {
        return;
    }
    sAssetLut = pc_load_file(ASSET_DIR "assets.lut.bin", &sAssetLutSize);
    sAssetsBin = pc_load_file(ASSET_DIR "assets.bin", &sAssetsBinSize);

    // LUT: entry count followed by offsets, all big-endian u32 — swap in place.
    for (i = 0; i + 3 < sAssetLutSize; i += 4) {
        u8 *p = &sAssetLut[i];
        u8 t0 = p[0], t1 = p[1];
        p[0] = p[3];
        p[1] = p[2];
        p[2] = t1;
        p[3] = t0;
    }
    printf("ASSETS: lut %u bytes, data %u bytes\n", sAssetLutSize, sAssetsBinSize);
}

u32 pc_asset_lut_size(void) {
    pc_assets_init();
    return sAssetLutSize;
}

// Byteswap a buffer of big-endian u32s in place (asset offset tables etc.).
// Endianness of asset *contents* is handled per parse site — this is the
// helper those sites share.
void pc_swap32_buf(void *buf, u32 numBytes) {
    u8 *p = buf;
    u32 i;

    for (i = 0; i + 3 < numBytes; i += 4) {
        u8 t0 = p[i + 0], t1 = p[i + 1];
        p[i + 0] = p[i + 3];
        p[i + 1] = p[i + 2];
        p[i + 2] = t1;
        p[i + 3] = t0;
    }
}

// Same, for buffers of big-endian u16s.
void pc_swap16_buf(void *buf, u32 numBytes) {
    u8 *p = buf;
    u32 i;

    for (i = 0; i + 1 < numBytes; i += 2) {
        u8 t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

// Byteswap a region of the loaded asset image in place, addressed the way the
// game addresses ROM. Audio's RAW16 sample data is big-endian s16 and is DMA'd
// straight out of this image on demand by __amDMA — which has no idea what type
// of wave it is serving — so the swap has to happen once, here, at the source.
// (ADPCM waves are a byte stream and must NOT come through this. See bnkf.c.)
void pc_asset_swap16_region(u32 romOffset, u32 numBytes) {
    u32 offset;

    pc_assets_init();

    if (romOffset < (u32) (uintptr_t) __ASSETS_LUT_END) {
        fprintf(stderr, "AUDIO: raw16 swap outside the asset image (0x%X) — skipped\n", romOffset);
        return;
    }
    offset = romOffset - (u32) (uintptr_t) __ASSETS_LUT_END;
    if (offset + numBytes > sAssetsBinSize) {
        fprintf(stderr, "AUDIO: raw16 swap past end of assets (0x%X + 0x%X) — skipped\n", offset, numBytes);
        return;
    }
    pc_swap16_buf(sAssetsBin + offset, numBytes);
}

void pc_dmacopy(u32 romOffset, u32 ramAddress, s32 numBytes) {
    pc_assets_init();

    if (romOffset == (u32) (uintptr_t) __ASSETS_LUT_START) {
        if ((u32) numBytes > sAssetLutSize) {
            numBytes = sAssetLutSize;
        }
        shz_memcpy((void *) (uintptr_t) ramAddress, sAssetLut, numBytes);
        return;
    }

    if (romOffset >= (u32) (uintptr_t) __ASSETS_LUT_END) {
        u32 offset = romOffset - (u32) (uintptr_t) __ASSETS_LUT_END;
        if (offset < sAssetsBinSize) {
            if (offset + numBytes > sAssetsBinSize) {
                fprintf(stderr, "ASSETS: read past end (offset 0x%X + 0x%X > 0x%X), clamped\n", offset, numBytes,
                        sAssetsBinSize);
                numBytes = sAssetsBinSize - offset;
            }
            shz_memcpy((void *) (uintptr_t) ramAddress, sAssetsBin + offset, numBytes);
            return;
        }
    }

    fprintf(stderr, "ASSETS: dmacopy from unknown ROM address 0x%X (%d bytes) — zero-filled\n", romOffset, numBytes);
    memset((void *) (uintptr_t) ramAddress, 0, numBytes);
}

// ---------------------------------------------------------------------------
// Boot globals (normally set up by the PIF/boot code)
// ---------------------------------------------------------------------------
void *osRomBase = (void *) 0xB0000000;
s32 osTvType = 1; // 0 = PAL, 1 = NTSC, 2 = MPAL
s32 osResetType = 0; // cold boot
s32 osAppNMIBuffer[16] = { 0 };

// Anti-piracy check: camera.c reads this raw cart-domain address and expects
// the low half to be 0x8965; anything else triggers the piracy path.
s32 D_B0000578 = 0x8965;

// ---------------------------------------------------------------------------
// Math library data (was libultra/src/gu/libm_vals.s)
// ---------------------------------------------------------------------------
float __libm_qnan_f = __builtin_nanf("");

// ---------------------------------------------------------------------------
// SI bus — controllers, EEPROM saves, controller paks, rumble.
// The N64 talks to all of these through PIF-RAM DMA over the Serial Interface
// (the libultra cont*/pfs*/eeprom*/motor files, dropped from the PC build).
// Constructive lies instead: one controller in port 0 with neutral input, an
// erased EEPROM (game falls back to creating default saves), and no
// controller paks / rumble paks plugged in anywhere.
// TODO: real host input (this is where it plugs in) and EEPROM persisted to a
// save file on disk.
// ---------------------------------------------------------------------------
#define PC_CONT_TYPE_NORMAL 0x0005
#define PC_CONT_NO_RESPONSE 0x8
#define PC_PFS_ERR_NOPACK 1
#define PC_MAXCONTROLLERS 4

typedef struct {
    u16 type;
    u8 status;
    u8 error;
} PCContStatus;

typedef struct {
    u16 button;
    s8 stick_x;
    s8 stick_y;
    u8 error;
} PCContPad;

extern s32 osSendMesg(void *mq, void *msg, s32 flags);

s32 osContInit(void *mq, u8 *bitpattern, PCContStatus *status) {
    s32 i;

    *bitpattern = 1; // controller in port 0 only
    for (i = 0; i < PC_MAXCONTROLLERS; i++) {
        status[i].type = (i == 0) ? PC_CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].error = (i == 0) ? 0 : PC_CONT_NO_RESPONSE;
    }
    return 0;
}

s32 osContStartReadData(void *mq) {
    // Poll "completes" instantly: post the done-message the game waits for.
    osSendMesg(mq, NULL, 0 /* OS_MESG_NOBLOCK */);
    return 0;
}

void osContGetReadData(PCContPad *pads) {
    s32 i;

    for (i = 0; i < PC_MAXCONTROLLERS; i++) {
        pads[i].button = 0;
        pads[i].stick_x = 0;
        pads[i].stick_y = 0;
        pads[i].error = (i == 0) ? 0 : PC_CONT_NO_RESPONSE;
    }

    input_host_read(&pads[0].button, &pads[0].stick_x, &pads[0].stick_y);
}

// EEPROM_TYPE_4K is 512 bytes / 64 eight-byte blocks.
#define PC_EEPROM_BLOCKS 64
#define PC_EEPROM_BYTES (PC_EEPROM_BLOCKS * 8)
#define VMU_SAVE_FILENAME "DKR_SAVE"

#ifdef EEPROM_PRESET_100
#include "eeprom_preset.h"
#endif

static u8 gEeprom[PC_EEPROM_BYTES];
static s32 gEepromReady = 0;
static s32 gEepromDirty = 0;
static s32 gPendingSaveFrames = 0;
static char gVmuSavePath[64] = "";

// 32x32 4bpp icon palette (16 ARGB4444 colors)
static const uint16_t sDkrVmuIconPal[16] = {
    0x0000, // 0: Transparent
    0xF000, // 1: Black
    0xFFFF, // 2: White
    0xFE00, // 3: Red
    0xFFC0, // 4: Yellow
    0xF08F, // 5: Blue
    0xF0B0, // 6: Dark Green
    0xF0F0, // 7: Green
    0xF840, // 8: Brown (Diddy Fur)
    0xFDAB, // 9: Peach/Tan (Face)
    0xFF00, // 10: Bright Gold / Star
    0xF888, // 11: Gray
    0xFCCC, // 12: Light Gray
    0xFF80, // 13: Orange
    0xFA00, // 14: Crimson Red
    0xF333  // 15: Dark Gray
};

// 32x32 4bpp icon data generated from kong_bits (512 bytes: Black & Yellow mapping)
static const uint8_t sDkrVmuIconData[512] = {
    0x11, 0x11, 0x41, 0x11, 0x11, 0x44, 0x41, 0x11, 0x11, 0x44, 0x41, 0x14, 0x44, 0x11, 0x44, 0x11,
    0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x44, 0x11, 0x44, 0x11,
    0x41, 0x14, 0x41, 0x14, 0x44, 0x11, 0x41, 0x14, 0x44, 0x11, 0x44, 0x14, 0x41, 0x14, 0x44, 0x11,
    0x41, 0x14, 0x41, 0x14, 0x44, 0x11, 0x41, 0x14, 0x44, 0x11, 0x44, 0x11, 0x11, 0x44, 0x44, 0x11,
    0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x44, 0x41, 0x14, 0x44, 0x44, 0x11,
    0x11, 0x11, 0x41, 0x11, 0x11, 0x44, 0x41, 0x11, 0x11, 0x44, 0x44, 0x41, 0x14, 0x44, 0x44, 0x11,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x41,
    0x11, 0x14, 0x44, 0x11, 0x11, 0x14, 0x44, 0x44, 0x44, 0x44, 0x44, 0x11, 0x14, 0x44, 0x44, 0x44,
    0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x14, 0x41, 0x11, 0x44, 0x44, 0x11, 0x14, 0x44, 0x41, 0x11,
    0x11, 0x41, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x14, 0x44, 0x11, 0x14, 0x44, 0x11, 0x11,
    0x11, 0x41, 0x11, 0x11, 0x14, 0x11, 0x11, 0x41, 0x11, 0x14, 0x44, 0x11, 0x14, 0x41, 0x11, 0x11,
    0x14, 0x41, 0x11, 0x11, 0x44, 0x11, 0x11, 0x41, 0x11, 0x11, 0x44, 0x11, 0x14, 0x41, 0x11, 0x11,
    0x14, 0x11, 0x11, 0x11, 0x44, 0x11, 0x11, 0x14, 0x11, 0x11, 0x14, 0x11, 0x14, 0x41, 0x11, 0x14,
    0x14, 0x11, 0x44, 0x44, 0x44, 0x41, 0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x14, 0x41, 0x11, 0x44,
    0x41, 0x11, 0x14, 0x44, 0x44, 0x44, 0x41, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x41, 0x11, 0x11, 0x44, 0x44, 0x44, 0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x14, 0x11, 0x11, 0x14, 0x44, 0x41, 0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x14, 0x41, 0x11, 0x44, 0x44, 0x41, 0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x11, 0x41, 0x11, 0x44, 0x14, 0x41, 0x11, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x11, 0x14, 0x14, 0x41, 0x11, 0x44, 0x11, 0x14, 0x11, 0x14, 0x11, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x11, 0x14, 0x11, 0x11, 0x11, 0x14, 0x11, 0x44, 0x11, 0x14, 0x41, 0x11, 0x11, 0x41, 0x11, 0x44,
    0x11, 0x11, 0x41, 0x11, 0x11, 0x11, 0x11, 0x44, 0x11, 0x14, 0x44, 0x11, 0x11, 0x41, 0x11, 0x14,
    0x11, 0x11, 0x41, 0x11, 0x11, 0x11, 0x14, 0x44, 0x11, 0x14, 0x44, 0x11, 0x11, 0x41, 0x11, 0x11,
    0x41, 0x14, 0x44, 0x11, 0x11, 0x14, 0x44, 0x44, 0x11, 0x14, 0x44, 0x41, 0x11, 0x41, 0x11, 0x11,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x11, 0x11,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44,
    0x11, 0x44, 0x44, 0x11, 0x14, 0x44, 0x41, 0x11, 0x11, 0x44, 0x11, 0x11, 0x41, 0x14, 0x44, 0x11,
    0x11, 0x14, 0x41, 0x11, 0x14, 0x44, 0x41, 0x44, 0x41, 0x14, 0x41, 0x14, 0x41, 0x11, 0x44, 0x11,
    0x11, 0x44, 0x41, 0x14, 0x11, 0x44, 0x11, 0x44, 0x44, 0x44, 0x41, 0x14, 0x41, 0x11, 0x44, 0x11,
    0x14, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x44, 0x44, 0x41, 0x14, 0x41, 0x11, 0x11, 0x11,
    0x11, 0x44, 0x11, 0x11, 0x41, 0x14, 0x41, 0x44, 0x41, 0x14, 0x41, 0x14, 0x41, 0x14, 0x11, 0x11,
    0x41, 0x14, 0x11, 0x44, 0x41, 0x14, 0x41, 0x11, 0x11, 0x44, 0x11, 0x11, 0x41, 0x14, 0x44, 0x11
};

// Helper: Find first connected VMU device and format target path into out_path
static s32 get_first_vmu_path(char *out_path, size_t max_len) {
    int i;
    for (i = 0; i < 8; i++) {
        maple_device_t *dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD);
        if (dev && dev->valid) {
            snprintf(out_path, max_len, "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, VMU_SAVE_FILENAME);
            return 1;
        }
    }
    return 0;
}

// Helper: Scan all attached VMUs to see if VMU_SAVE_FILENAME already exists
static s32 find_existing_save_vmu(char *out_path, size_t max_len) {
    int i;
    for (i = 0; i < 8; i++) {
        maple_device_t *dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD);
        if (dev && dev->valid) {
            char candidate[64];
            snprintf(candidate, sizeof(candidate), "/vmu/%c%d/%s", 'a' + dev->port, dev->unit, VMU_SAVE_FILENAME);
            file_t fd = fs_open(candidate, O_RDONLY);
            if (fd >= 0) {
                fs_close(fd);
                snprintf(out_path, max_len, "%s", candidate);
                return 1;
            }
        }
    }
    return 0;
}

static void eeprom_init(void) {
    file_t fd;
    if (gEepromReady) {
        return;
    }
    gEepromReady = 1;

    if (find_existing_save_vmu(gVmuSavePath, sizeof(gVmuSavePath))) {
        fd = fs_open(gVmuSavePath, O_RDONLY);
        if (fd >= 0) {
            ssize_t read_bytes = fs_read(fd, gEeprom, PC_EEPROM_BYTES);
            fs_close(fd);
            if (read_bytes == PC_EEPROM_BYTES) {
                printf("[VMU] Successfully loaded 512 bytes EEPROM save from %s\n", gVmuSavePath);
                gEepromDirty = 0;
                return;
            } else {
                printf("[VMU] Warning: Read %d bytes (expected 512) from %s\n", (int)read_bytes, gVmuSavePath);
            }
        }
    }

    // No existing save found or read failed: set up target path & default save buffer
    if (!get_first_vmu_path(gVmuSavePath, sizeof(gVmuSavePath))) {
        gVmuSavePath[0] = '\0';
    }

#ifdef EEPROM_PRESET_100
    memcpy(gEeprom, gEepromPreset, PC_EEPROM_BYTES);
    printf("[VMU] No VMU save found. Loaded 100%% preset into memory (target: %s)\n",
           gVmuSavePath[0] ? gVmuSavePath : "None");
#else
    memset(gEeprom, 0xFF, PC_EEPROM_BYTES); // erased
    printf("[VMU] No VMU save found. Initialized erased EEPROM in memory (target: %s)\n",
           gVmuSavePath[0] ? gVmuSavePath : "None");
#endif
    gEepromDirty = 0;
}

int eeprom_flush_to_vmu(void) {
    vmu_pkg_t pkg;
    file_t fd;
    ssize_t written;

    if (!gEepromDirty) {
        return 0;
    }

    if (gVmuSavePath[0] == '\0') {
        if (!get_first_vmu_path(gVmuSavePath, sizeof(gVmuSavePath))) {
            printf("[VMU] Cannot save: No VMU detected!\n");
            return -1;
        }
    }

    fd = fs_open(gVmuSavePath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        // If save path failed (e.g. VMU unplugged), try finding another connected VMU
        if (get_first_vmu_path(gVmuSavePath, sizeof(gVmuSavePath))) {
            fd = fs_open(gVmuSavePath, O_WRONLY | O_CREAT | O_TRUNC);
        }
    }

    if (fd < 0) {
        printf("[VMU] Error: Failed to open %s for writing\n", gVmuSavePath);
        return -1;
    }

    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "DiddyKongRacing", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long, "Diddy Kong Racing Save", sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id, "DKR", sizeof(pkg.app_id) - 1);
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = 0;
    memcpy(pkg.icon_pal, sDkrVmuIconPal, sizeof(sDkrVmuIconPal));
    pkg.icon_data = (uint8_t *)sDkrVmuIconData;
    pkg.data_len = PC_EEPROM_BYTES;
    pkg.data = gEeprom;

    fs_vmu_set_header(fd, &pkg);
    written = fs_write(fd, gEeprom, PC_EEPROM_BYTES);
    fs_close(fd);

    if (written == PC_EEPROM_BYTES) {
        printf("[VMU] Successfully saved 512 bytes EEPROM to %s\n", gVmuSavePath);
        gEepromDirty = 0;
        gPendingSaveFrames = 0;
        return 0;
    } else {
        printf("[VMU] Error: Wrote %d bytes (expected 512) to %s\n", (int)written, gVmuSavePath);
        return -1;
    }
}

void eeprom_update(void) {
    if (gEepromDirty) {
        if (gPendingSaveFrames > 0) {
            gPendingSaveFrames--;
        } else {
            eeprom_flush_to_vmu();
        }
    }
}

s32 osEepromProbe(void *mq) {
    eeprom_init();
    return 1; // EEPROM_TYPE_4K — present
}

s32 osEepromRead(void *mq, u8 address, u8 *buffer) {
    eeprom_init();
    if (address >= PC_EEPROM_BLOCKS) {
        memset(buffer, 0xFF, 8);
        return 0;
    }
    memcpy(buffer, &gEeprom[address * 8], 8);
    return 0;
}

s32 osEepromWrite(void *mq, u8 address, u8 *buffer) {
    eeprom_init();
    if (address < PC_EEPROM_BLOCKS) {
        if (memcmp(&gEeprom[address * 8], buffer, 8) != 0) {
            memcpy(&gEeprom[address * 8], buffer, 8);
            gEepromDirty = 1;
            gPendingSaveFrames = 30; // batch write 30 frames (~0.5s) after last block edit
        }
    }
    return 0;
}

s32 osPfsInit(void *mq, void *pfs, s32 channel) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsIsPlug(void *mq, u8 *pattern) {
    *pattern = 0;
    return 0;
}

s32 osPfsFileState(void *pfs, s32 fileNo, void *state) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsReadWriteFile(void *pfs, s32 fileNo, u8 flag, s32 offset, s32 size, u8 *data) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsNumFiles(void *pfs, s32 *maxFiles, s32 *filesUsed) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsFreeBlocks(void *pfs, s32 *bytesNotUsed) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsReFormat(void *pfs, void *mq, s32 channel) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsFindFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, s32 *fileNo) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsDeleteFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsChecker(void *pfs) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsAllocateFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, s32 size, s32 *fileNo) {
    return PC_PFS_ERR_NOPACK;
}

s32 osMotorInit(void *mq, void *pfs, s32 channel) {
    return PC_PFS_ERR_NOPACK; // no rumble pak
}

s32 osMotorStart(void *pfs) {
    return 0;
}

s32 osMotorStop(void *pfs) {
    return 0;
}

// ---------------------------------------------------------------------------
// Debug printing (was src/isv_print.c writing to the IS-Viewer MMIO device at
// 0xB3FF0000 — the game's stubbed_printf channel; goes to stdout here)
// ---------------------------------------------------------------------------
void isv_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    // Flush every line: this is the port's only debug channel, and on a crash or
    // an ASan abort a buffered stdout swallows exactly the lines that say why.
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Cache management (no caches to manage on the host)
// ---------------------------------------------------------------------------
void osWritebackDCache(void *vaddr, s32 size) {}
void osWritebackDCacheAll(void) {}
void osInvalDCache(void *vaddr, s32 size) {}
void osInvalICache(void *vaddr, s32 size) {}

// ---------------------------------------------------------------------------
// Interrupt management
//
// __osDisableInt/__osRestoreInt guard the libultra message-queue layer
// (osSendMesg/osRecvMesg). Those stay no-ops: the queue traffic is not shared
// across the host's threads in a way that races.
//
// osSetIntMask is different. On N64 it is used by ONE subsystem — audiosfx.c —
// and only there, to fence the game thread's sound-effect state against the
// audio thread that consumes it (7 balanced OS_IM_NONE..restore pairs). Now
// that DKR's audio manager runs on its own KOS thread again (dreamcast/audio.c),
// that fence has to be real mutual exclusion, not a no-op. Back it with a
// recursive mutex the audio thread also holds while it runs the synth, so a
// game-thread SFX post can't tear state out from under __amHandleFrameMsg.
// ---------------------------------------------------------------------------
#define PC_OS_IM_NONE 0x00000001u

static mutex_t sAudioMutex = RECURSIVE_MUTEX_INITIALIZER;

// Held by the audio thread across am_audio_frame_pc(); see dreamcast/audio.c.
void pc_audio_lock(void) {
    mutex_lock(&sAudioMutex);
}
void pc_audio_unlock(void) {
    mutex_unlock(&sAudioMutex);
}

s32 __osDisableInt(void) {
    return 0;
}
void __osRestoreInt(s32 mask) {}

u32 osSetIntMask(u32 mask) {
    // osSetIntMask(OS_IM_NONE) enters a critical section; the paired call passes
    // back the saved mask (our 0) to leave it. Only unlock when this thread is
    // actually the holder, so a stray "enable" that was never paired with an
    // OS_IM_NONE can't underflow the mutex.
    if (mask == PC_OS_IM_NONE) {
        mutex_lock(&sAudioMutex);
    } else if (sAudioMutex.holder == thd_get_current()) {
        mutex_unlock(&sAudioMutex);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Timer / CP0 / FPU registers
// ---------------------------------------------------------------------------
// The N64 Count register ticks at 46.875 MHz (CPU clock / 2).
// 46875000 ticks/sec over 1e9 ns/sec = exactly 3/64 ticks per nanosecond.
u32 osGetCount(void) {
    u64 ns = timer_ns_gettime64();
    return (u32) (ns * 3 / 64);
}

u64 pc_uptime_ns(void) {
    return timer_ns_gettime64();
}
void __osSetCompare(u32 value) {}
u32 __osGetSR(void) {
    return 0;
}
void __osSetSR(u32 value) {}
u32 __osSetFpcCsr(u32 value) {
    return 0;
}

// ---------------------------------------------------------------------------
// TLB (host has a real MMU; virtual==physical as far as the game cares)
// ---------------------------------------------------------------------------
u32 __osProbeTLB(void *vaddr) {
    return 0;
}
void osMapTLBRdb(void) {}

// ---------------------------------------------------------------------------
// Exception / hardware-interrupt plumbing (was exceptasm.s data)
// ---------------------------------------------------------------------------
typedef struct {
    unsigned int inst1;
    unsigned int inst2;
    unsigned int inst3;
    unsigned int inst4;
} __osExceptionVector;
__osExceptionVector __osExceptionPreamble[1] = { { 0, 0, 0, 0 } };

// Pre-2.0J layout (both PC targets build VERSION_G): a plain array of
// handlers, with no stack frame.
s32 (*__osHwIntTable[8])(void) = { 0 };

// ---------------------------------------------------------------------------
// Thread context switching (was exceptasm.s code).
// Empty stubs for now.
// ---------------------------------------------------------------------------
void __osEnqueueThread(void **queue, void *thread) {}
void __osEnqueueAndYield(void **queue) {}
void *__osPopThread(void **queue) {
    return 0;
}
void __osDispatchThread(void) {}
void __osCleanupThread(void) {}
