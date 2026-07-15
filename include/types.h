#ifndef _TYPES_H_
#define _TYPES_H_

#include <PR/ultratypes.h>

typedef float MtxF[4][4];
typedef s32 MtxS[4][4];
typedef s16 VertexList;
typedef u8 TriangleList;
// Platform code (dreamcast/, linux/) pulls the system headers, whose newlib
// (DC) already declares uintptr_t and sets this guard; redefining it is an
// error under C23. Game code compiles against the N64 libc stubs, which never
// declare it, so the typedef below still applies there.
#ifndef _UINTPTR_T_DECLARED
typedef u32 uintptr_t;
#define _UINTPTR_T_DECLARED
#endif

#ifdef NON_MATCHING
void isv_printf(const char *fmt, ...);
#define stubbed_printf isv_printf
#else
#define stubbed_printf
#endif

#endif
