#ifndef _TYPES_H_
#define _TYPES_H_

#include <PR/ultratypes.h>

typedef float MtxF[4][4];
typedef s32 MtxS[4][4];
typedef s16 VertexList;
typedef u8 TriangleList;
typedef u32 uintptr_t;

#ifdef NON_MATCHING
void isv_printf(const char *fmt, ...);
#define stubbed_printf isv_printf
#else
#define stubbed_printf
#endif

#endif
