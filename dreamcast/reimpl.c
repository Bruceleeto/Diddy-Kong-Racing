// OS reimplementation stubs for the PC build.
// Modeled on the OoT DC port's src/linux/reimpl.c — same names and behavior
// where the two games needed the same symbol; DKR-specific ones added at the
// bottom of each section.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "input.h"

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
u8 __ROM_END[1] = { 0 }; // cheat-menu checksum bound
u8 *main_BSS_START[1] = { 0 };

// N64: the main heap is [end of BSS .. RAM_END], carved out by the linker.
// PC: a static pool. memory.c's "ramEnd - (s32)&gMainMemoryPool" sizing math
// still needs TARGET_PC surgery to use this pool's real size instead.
u8 gMainMemoryPool[16 * 1024 * 1024] __attribute__((aligned(16)));
u32 gMainMemoryPoolSize = sizeof(gMainMemoryPool);

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
// On Dreamcast the assets ride on the disc, which KOS mounts at /cd. On the PC
// build they sit in the repo's assets/ dir, read relative to the launch cwd.
#ifdef TARGET_DC
#define ASSET_DIR "/cd/assets/"
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
        memcpy((void *) (uintptr_t) ramAddress, sAssetLut, numBytes);
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
            memcpy((void *) (uintptr_t) ramAddress, sAssetsBin + offset, numBytes);
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

s32 osEepromProbe(void *mq) {
    return 1; // EEPROM_TYPE_4K — present
}

s32 osEepromRead(void *mq, u8 address, u8 *buffer) {
    memset(buffer, 0xFF, 8); // erased EEPROM block
    return 0;
}

s32 osEepromWrite(void *mq, u8 address, u8 *buffer) {
    return 0; // accepted, not persisted (yet)
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

s32 osPfsFindFile(void *pfs, u16 companyCode, u32 gameCode, u16 *gameName, u16 *extName, s32 *fileNo) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsDeleteFile(void *pfs, u16 companyCode, u32 gameCode, u16 *gameName, u16 *extName) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsChecker(void *pfs) {
    return PC_PFS_ERR_NOPACK;
}

s32 osPfsAllocateFile(void *pfs, u16 companyCode, u32 gameCode, u16 *gameName, u16 *extName, s32 size, s32 *fileNo) {
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
// Interrupt management (single-threaded host: nothing to mask)
// ---------------------------------------------------------------------------
s32 __osDisableInt(void) {
    return 0;
}
void __osRestoreInt(s32 mask) {}
u32 osSetIntMask(u32 mask) {
    return 0;
}

// ---------------------------------------------------------------------------
// Timer / CP0 / FPU registers
// ---------------------------------------------------------------------------
// The N64 Count register ticks at 46.875 MHz (CPU clock / 2).
// 46875000 ticks/sec over 1e9 ns/sec = exactly 3/64 ticks per nanosecond.
u32 osGetCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 ns = (u64) ts.tv_sec * 1000000000ull + (u64) ts.tv_nsec;
    return (u32) (ns * 3 / 64);
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
u32 __osExceptionPreamble[1] = { 0 };

struct __osHwInt {
    s32 (*handler)(void);
    void *stackEnd;
};
struct __osHwInt __osHwIntTable[8] = { 0 };

// ---------------------------------------------------------------------------
// Thread context switching (was exceptasm.s code).
// Empty stubs for now, same as the OoT port's bring-up state — the real
// scheduler replacement is its own milestone.
// ---------------------------------------------------------------------------
void __osEnqueueThread(void **queue, void *thread) {}
void __osEnqueueAndYield(void **queue) {}
void *__osPopThread(void **queue) {
    return 0;
}
void __osDispatchThread(void) {}
void __osCleanupThread(void) {}
