#include "types.h"



s64 __fixsfdi(f32 f) {
    union {
        f32 f;
        u32 u;
    } uf;
    u32 mantissa;
    s32 exp;
    s32 sign;
    s32 shift;
    u64 m;
    s64 result;

    uf.f = f;
    sign = (uf.u >> 31) & 1;
    exp = (s32) ((uf.u >> 23) & 0xFF);
    mantissa = uf.u & 0x7FFFFF;

    if (exp == 0 || exp == 0xFF) {
        // Zero, subnormal (rounds to zero for any value we deal with), or inf/nan.
        return 0;
    }

    m = ((u64) mantissa) | 0x800000ULL;
    shift = exp - 127 - 23;

    if (shift >= 0) {
        if (shift >= 40) {
            return sign ? (s64) 0x8000000000000000LL : (s64) 0x7FFFFFFFFFFFFFFFLL;
        }
        result = (s64) (m << shift);
    } else {
        shift = -shift;
        if (shift >= 64) {
            return 0;
        }
        result = (s64) (m >> shift);
    }

    return sign ? -result : result;
}

f32 __floatdisf(s64 x) {
    union {
        u32 u;
        f32 f;
    } uf;
    u64 m;
    s32 sign;
    s32 msb;
    s32 shift;
    u32 mantissa;
    u32 exp;

    if (x == 0) {
        return 0.0f;
    }

    sign = (x < 0);
    m = sign ? (u64) (-x) : (u64) x;

    msb = 63;
    while (!(m & ((u64) 1 << msb))) {
        msb--;
    }

    if (msb >= 23) {
        shift = msb - 23;
        mantissa = (u32) ((m >> shift) & 0x7FFFFF);
        // Round to nearest based on the highest bit being shifted out.
        if (m & ((u64) 1 << (shift - 1))) {
            mantissa++;
            if (mantissa & 0x800000) {
                mantissa = 0;
                msb++;
            }
        }
    } else {
        shift = 23 - msb;
        mantissa = (u32) ((m << shift) & 0x7FFFFF);
    }

    exp = (u32) (msb + 127);
    uf.u = ((u32) sign << 31) | (exp << 23) | mantissa;
    return uf.f;
}
