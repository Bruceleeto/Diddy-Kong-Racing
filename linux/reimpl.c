// OS reimplementation stubs for the PC build.
// Modeled on the OoT DC port's src/linux/reimpl.c — same names and behavior
// where the two games needed the same symbol; DKR-specific ones added at the
// bottom of each section.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef signed char s8;
typedef signed int s32;
typedef unsigned char u8;
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
// Debug printing (was src/isv_print.c writing to the IS-Viewer MMIO device at
// 0xB3FF0000 — the game's stubbed_printf channel; goes to stdout here)
// ---------------------------------------------------------------------------
void isv_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
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
