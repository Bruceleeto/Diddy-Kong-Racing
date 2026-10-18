#include "dkr_asset_mount.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include <sh4zam/shz_sh4zam.h>

#include "input.h"
#include "vmu_icon.h"

typedef signed char s8;
typedef signed int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

// ---------------------------------------------------------------------------
// RSP microcode blobs
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
// Linker-script symbols
// ---------------------------------------------------------------------------
u8 __ASSETS_LUT_START[1] = { 0 };
u8 __ASSETS_LUT_END[1] = { 0 };
u8 *__ROM_END;
u8 *main_BSS_START[1] = { 0 };

// ---------------------------------------------------------------------------
// Asset "DMA" con Streaming desde CD (Ahorro de 11.14 MB de RAM)
// ---------------------------------------------------------------------------
#ifdef TARGET_DC
#ifdef DKR_ASSET_MOUNT
#define ASSET_DIR DKR_ASSET_MOUNT "/assets/"
#else
#define ASSET_DIR "/cd/assets/"
#endif
#else
#define ASSET_DIR "assets/"
#endif

static u8 *sAssetLut = NULL;
static u32 sAssetLutSize = 0;
static u8 *sAssetsBin = NULL;
static FILE *sAssetsFile = NULL;
static u32 sAssetsBinSize = 0;
static u8 sVmuReadBuf[2048];

static u8 *pc_load_file(const char *path, u32 *sizeOut) {
    FILE *f = fopen(path, "rb");
    long size;
    u8 *buf;

    if (f == NULL) {
        fprintf(stderr, "ASSETS: cannot open %s (run from the repo root, and build the N64 assets first)\n", path);
        exit(1);
    }
    setvbuf(f, NULL, _IONBF, 0);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "ASSETS: allocation failed for %s (%ld bytes)\n", path, size);
        fclose(f);
        exit(1);
    }
    if (fread(buf, 1, size, f) != (size_t) size) {
        fprintf(stderr, "ASSETS: short read on %s\n", path);
        fclose(f);
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

    // Apertura en modo streaming para evitar malloc masivo en los 16 MB de RAM
    sAssetsFile = fopen(ASSET_DIR "assets.bin", "rb");
    if (sAssetsFile == NULL) {
        sAssetsFile = fopen("/cd/assets/assets.bin", "rb");
    }
    if (sAssetsFile != NULL) {
        fseek(sAssetsFile, 0, SEEK_END);
        sAssetsBinSize = (u32) ftell(sAssetsFile);
        fseek(sAssetsFile, 0, SEEK_SET);
    }

    for (i = 0; i + 3 < sAssetLutSize; i += 4) {
        u8 *p = &sAssetLut[i];
        u8 t0 = p[0], t1 = p[1];
        p[0] = p[3];
        p[1] = p[2];
        p[2] = t1;
        p[3] = t0;
    }
    printf("ASSETS: lut %u bytes, data %u bytes (streaming activo)\n", sAssetLutSize, sAssetsBinSize);
}

u32 pc_asset_lut_size(void) {
    pc_assets_init();
    return sAssetLutSize;
}

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

void pc_swap16_buf(void *buf, u32 numBytes) {
    u8 *p = buf;
    u32 i;

    for (i = 0; i + 1 < numBytes; i += 2) {
        u8 t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

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
    if (sAssetsBin != NULL) {
        pc_swap16_buf(sAssetsBin + offset, numBytes);
    }
}

void pc_dmacopy(u32 romOffset, u32 ramAddress, s32 numBytes) {
    // Protección contra tamaños negativos o cero
    if (numBytes <= 0) return;
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
            // Lectura directa desde disco sin almacenar en RAM
            if (sAssetsFile != NULL) {
                fseek(sAssetsFile, offset, SEEK_SET);
                fread((void *) (uintptr_t) ramAddress, 1, numBytes, sAssetsFile);
            } else if (sAssetsBin != NULL) {
                shz_memcpy((void *) (uintptr_t) ramAddress, sAssetsBin + offset, numBytes);
            }
            return;
        }
    }

    fprintf(stderr, "ASSETS: dmacopy from unknown ROM address 0x%X (%d bytes) — zero-filled\n", romOffset, numBytes);
    memset((void *) (uintptr_t) ramAddress, 0, numBytes);
}

// ---------------------------------------------------------------------------
// Boot globals
// ---------------------------------------------------------------------------
void *osRomBase = (void *) 0xB0000000;
s32 osTvType = 1;
s32 osResetType = 0;
s32 osAppNMIBuffer[16] = { 0 };
s32 D_B0000578 = 0x8965;
float __libm_qnan_f = __builtin_nanf("");

// ---------------------------------------------------------------------------
// SI bus — Mandos
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
    *bitpattern = 0x0F; // los 4 puertos habilitados
    for (i = 0; i < PC_MAXCONTROLLERS; i++) {
        status[i].type = PC_CONT_TYPE_NORMAL;
        status[i].status = 0;
        status[i].error = 0;
    }
    return 0;
}

s32 osContStartReadData(void *mq) {
    osSendMesg(mq, NULL, 0);
    return 0;
}

void osContGetReadData(PCContPad *pads) {
    s32 i;

    for (i = 0; i < PC_MAXCONTROLLERS; i++) {
        pads[i].button = 0;
        pads[i].stick_x = 0;
        pads[i].stick_y = 0;
        pads[i].error = 0;
        input_host_read(i, &pads[i].button, &pads[i].stick_x, &pads[i].stick_y);
    }
}

// ---------------------------------------------------------------------------
// EEPROM / Guardado en VMU (Sin icono/logo)
// ---------------------------------------------------------------------------
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
static s32 gVmuChecked = 0; // 1 si al arrancar habia VMU y ya se comprobo su guardado

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
    maple_wait_scan(); // espera al escaneo inicial de Maple (deteccion fiable de la VMU)

    if (find_existing_save_vmu(gVmuSavePath, sizeof(gVmuSavePath))) {
        fd = fs_open(gVmuSavePath, O_RDONLY);
        if (fd >= 0) {
			// Leemos hasta 2048 bytes: un archivo VMS con cabecera ocupa 128 + datos
            ssize_t n = fs_read(fd, sVmuReadBuf, sizeof(sVmuReadBuf));
            fs_close(fd);

            int loaded = 0;
			// La cabecera VMS empieza con la descripcion corta que escribimos al guardar
            int hasHeader = (n >= 128 && memcmp(sVmuReadBuf, "DiddyKongRacing", 15) == 0);
            printf("[VMU] Leidos %d bytes de %s (cabecera VMS: %s)\n",
                   (int)n, gVmuSavePath, hasHeader ? "si" : "no");

            if (hasHeader) {
                vmu_pkg_t pkg;
                if (vmu_pkg_parse(sVmuReadBuf, (size_t)n, &pkg) == 0
                    && pkg.data_len == PC_EEPROM_BYTES) {
                    memcpy(gEeprom, pkg.data, PC_EEPROM_BYTES);
                    loaded = 1;
                } else {
                  // El parser de KOS no la acepta: sacamos los datos a mano.
                 // Van tras los 0x80 bytes de cabecera + 512 por cada icono.
                    size_t icons = sVmuReadBuf[0x40] | (sVmuReadBuf[0x41] << 8);
                    size_t off = 0x80 + icons * 512;
                    if (off + PC_EEPROM_BYTES <= (size_t)n) {
                        memcpy(gEeprom, sVmuReadBuf + off, PC_EEPROM_BYTES);
                        loaded = 1;
                    }
                }
            } else if (n >= PC_EEPROM_BYTES) {
                // KOS entrego solo los datos, sin cabecera VMS
                memcpy(gEeprom, sVmuReadBuf, PC_EEPROM_BYTES);
                loaded = 1;
            }

            if (loaded) {
                printf("[VMU] Partida cargada (%d bytes) desde %s\n",
                       PC_EEPROM_BYTES, gVmuSavePath);
                gEepromDirty = 0;
                gVmuChecked = 1;
                return;
            }
            printf("[VMU] Partida no valida en %s, iniciando por defecto\n", gVmuSavePath);
        }
    }

    if (!get_first_vmu_path(gVmuSavePath, sizeof(gVmuSavePath))) {
        gVmuSavePath[0] = '\0';
    } else {
        gVmuChecked = 1;
    }

#ifdef EEPROM_PRESET_100
    memcpy(gEeprom, gEepromPreset, PC_EEPROM_BYTES);
#else
    memset(gEeprom, 0xFF, PC_EEPROM_BYTES);
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
            gPendingSaveFrames = 300;// sin VMU: reintenta en ~10 s, no en cada frame
            return -1;
        }
    }

    if (!gVmuChecked) {
        // VMU insertada tras el arranque: no pisar un guardado que nunca se leyo
        char existing[64];
        if (find_existing_save_vmu(existing, sizeof(existing))) {
            printf("[VMU] Hay una partida en la VMU insertada tarde, no se sobrescribe\n");
            gPendingSaveFrames = 1800;
            return -1;
        }
        gVmuChecked = 1;
    }

    fd = fs_open(gVmuSavePath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        if (get_first_vmu_path(gVmuSavePath, sizeof(gVmuSavePath))) {
            fd = fs_open(gVmuSavePath, O_WRONLY | O_CREAT | O_TRUNC);
        }
    }

    if (fd < 0) {
        return -1;
    }

    // Cabecera limpia SIN logo ni paleta de icono
    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "DiddyKongRacing", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long, "Diddy Kong Racing Save", sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id, "DKR", sizeof(pkg.app_id) - 1);
    pkg.icon_cnt = 1;          // un icono de 32x32 (4 bpp)
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = 0;
    memcpy(pkg.icon_pal, sDkrVmuIconPal, sizeof(sDkrVmuIconPal)); // 16 colores = 32 bytes
    pkg.icon_data = (uint8_t *)sDkrVmuIconData;                   // 512 bytes
    pkg.data_len = PC_EEPROM_BYTES;
    pkg.data = gEeprom;

    fs_vmu_set_header(fd, &pkg);
    written = fs_write(fd, gEeprom, PC_EEPROM_BYTES);
    fs_close(fd);

    if (written == PC_EEPROM_BYTES) {
        printf("[VMU] Guardado correcto en %s\n", gVmuSavePath);
        gEepromDirty = 0;
        gPendingSaveFrames = 0;
        return 0;
    }

    return -1;
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
    return 1;
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
            gPendingSaveFrames = 30; // Guarda 30 frames tras la última edición
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Controller Pak Stubs
// ---------------------------------------------------------------------------
s32 osPfsInit(void *mq, void *pfs, s32 channel) { return PC_PFS_ERR_NOPACK; }
s32 osPfsIsPlug(void *mq, u8 *pattern) { *pattern = 0; return 0; }
s32 osPfsFileState(void *pfs, s32 fileNo, void *state) { return PC_PFS_ERR_NOPACK; }
s32 osPfsReadWriteFile(void *pfs, s32 fileNo, u8 flag, s32 offset, s32 size, u8 *data) { return PC_PFS_ERR_NOPACK; }
s32 osPfsNumFiles(void *pfs, s32 *maxFiles, s32 *filesUsed) { return PC_PFS_ERR_NOPACK; }
s32 osPfsFreeBlocks(void *pfs, s32 *bytesNotUsed) { return PC_PFS_ERR_NOPACK; }
s32 osPfsReFormat(void *pfs, void *mq, s32 channel) { return PC_PFS_ERR_NOPACK; }
s32 osPfsFindFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, s32 *fileNo) { return PC_PFS_ERR_NOPACK; }
s32 osPfsDeleteFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName) { return PC_PFS_ERR_NOPACK; }
s32 osPfsChecker(void *pfs) { return PC_PFS_ERR_NOPACK; }
s32 osPfsAllocateFile(void *pfs, u16 companyCode, u32 gameCode, u8 *gameName, u8 *extName, s32 size, s32 *fileNo) { return PC_PFS_ERR_NOPACK; }
s32 osMotorInit(void *mq, void *pfs, s32 channel) { return PC_PFS_ERR_NOPACK; }
s32 osMotorStart(void *pfs) { return 0; }
s32 osMotorStop(void *pfs) { return 0; }

// ---------------------------------------------------------------------------
// Debug & Cache
// ---------------------------------------------------------------------------
void isv_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void osWritebackDCache(void *vaddr, s32 size) {}
void osWritebackDCacheAll(void) {}
void osInvalDCache(void *vaddr, s32 size) {}
void osInvalICache(void *vaddr, s32 size) {}

// ---------------------------------------------------------------------------
// Interrupt management
// ---------------------------------------------------------------------------
#define PC_OS_IM_NONE 0x00000001u

static mutex_t sAudioMutex = RECURSIVE_MUTEX_INITIALIZER;

void pc_audio_lock(void) {
    mutex_lock(&sAudioMutex);
}
void pc_audio_unlock(void) {
    mutex_unlock(&sAudioMutex);
}

s32 __osDisableInt(void) { return 0; }
void __osRestoreInt(s32 mask) {}

u32 osSetIntMask(u32 mask) {
    if (mask == PC_OS_IM_NONE) {
        mutex_lock(&sAudioMutex);
    } else if (sAudioMutex.holder == thd_get_current()) {
        mutex_unlock(&sAudioMutex);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Timer / CP0 / FPU
// ---------------------------------------------------------------------------
u32 osGetCount(void) {
    u64 ns = timer_ns_gettime64();
    return (u32) (ns * 3 / 64);
}

u64 pc_uptime_ns(void) {
    return timer_ns_gettime64();
}
void __osSetCompare(u32 value) {}
u32 __osGetSR(void) { return 0; }
void __osSetSR(u32 value) {}
u32 __osSetFpcCsr(u32 value) { return 0; }
u32 __osProbeTLB(void *vaddr) { return 0; }
void osMapTLBRdb(void) {}

typedef struct {
    unsigned int inst1;
    unsigned int inst2;
    unsigned int inst3;
    unsigned int inst4;
} __osExceptionVector;
__osExceptionVector __osExceptionPreamble[1] = { { 0, 0, 0, 0 } };

s32 (*__osHwIntTable[8])(void) = { 0 };

void __osEnqueueThread(void **queue, void *thread) {}
void __osEnqueueAndYield(void **queue) {}
void *__osPopThread(void **queue) { return 0; }
void __osDispatchThread(void) {}
void __osCleanupThread(void) {}