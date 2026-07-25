
#include <string.h>

#include <sh4zam/shz_sh4zam.h>

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed long long s64;

// ---------------------------------------------------------------------------
// ABI1 opcodes (include/PR/abi.h)
// ---------------------------------------------------------------------------
#define A_SPNOOP 0
#define A_ADPCM 1
#define A_CLEARBUFF 2
#define A_ENVMIXER 3
#define A_LOADBUFF 4
#define A_RESAMPLE 5
#define A_SAVEBUFF 6
#define A_SEGMENT 7
#define A_SETBUFF 8
#define A_SETVOL 9
#define A_DMEMMOVE 10
#define A_LOADADPCM 11
#define A_MIXER 12
#define A_INTERLEAVE 13
#define A_POLEF 14
#define A_SETLOOP 15

// Flags
#define A_INIT 0x01
#define A_CONTINUE 0x00
#define A_LOOP 0x02
#define A_LEFT 0x02
#define A_RIGHT 0x00
#define A_VOL 0x04
#define A_RATE 0x00
#define A_AUX 0x08

#define DMEM_SIZE 4096

#undef SHZ_PREFETCH
#define SHZ_PREFETCH(ptr) asm("pref @%0" : : "r" (ptr))

// ---------------------------------------------------------------------------
// RSP state
// ---------------------------------------------------------------------------
static struct {
    alignas(32) u8 dmem[DMEM_SIZE];

    u16 in, out, count;              // SETBUFF, no flags
    u16 dryRight, wetLeft, wetRight; // SETBUFF | A_AUX
    s16 vol[2];                      // [0]=left, [1]=right
    s16 target[2];
    s32 rate[2];
    s16 dry, wet;
    u32 loopAddr;

    alignas(32) float tableF[8][2][8];
    alignas(32) s16 table[512]; // ADPCM codebook, also POLEF coefficients
} rspa;

// Identity "physical" addresses: osVirtualToPhysical is identity on the host, so a
// DMA address in the command list is just a host pointer.
SHZ_FORCE_INLINE
void *rdram(u32 addr) {
    return (void *) (unsigned long) addr;
}

SHZ_FORCE_INLINE
s16 clamp16(s32 v) {
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (s16) v;
}

SHZ_FORCE_INLINE
s16 clamp16f(float v) {
    if (v < -32768.0f) {
        return -32768;
    }
    if (v > 32767.0f) {
        return 32767;
    }
    return (s16) v;
}

SHZ_FORCE_INLINE
float fclamp16f(float v, s16* res) {
    float f;

    if (v < -32768.0f)
        f = -32768.0f;
    else if (v > 32767.0f)
        f = 32767.0f;
    else
        f = v;

    *res = (s16)f;
    return f;
}

// DMEM is byte-addressed but every audio buffer in it is s16-aligned.
static s16 *dmem16(u32 addr) {
    return (s16 *) &rspa.dmem[addr & (DMEM_SIZE - 2)];
}

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

static void op_setbuff(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;

    if (flags & A_AUX) {
        // The aux form reuses the three fields as three DMEM addresses — see
        // alMainBusPull: aSetBuffer(A_AUX, AL_MAIN_R_OUT, AL_AUX_L_OUT, AL_AUX_R_OUT).
        // The last one is NOT a count here.
        rspa.dryRight = w0 & 0xFFFF;
        rspa.wetLeft = (w1 >> 16) & 0xFFFF;
        rspa.wetRight = w1 & 0xFFFF;
    } else {
        rspa.in = w0 & 0xFFFF;
        rspa.out = (w1 >> 16) & 0xFFFF;
        rspa.count = w1 & 0xFFFF;
    }
}

static void op_setvol(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 v = (s16) (w0 & 0xFFFF);
    u16 t = (w1 >> 16) & 0xFFFF;
    u16 r = w1 & 0xFFFF;

    if (flags & A_AUX) {
        rspa.dry = v;
        rspa.wet = (s16) r;
    } else if (flags & A_VOL) {
        rspa.vol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        // A_RATE: v is the envelope target, (t:r) the 16.16 per-sample step.
        s32 idx = (flags & A_LEFT) ? 0 : 1;
        rspa.target[idx] = v;
        rspa.rate[idx] = (s32) (((u32) t << 16) | r);
    }
}

static void op_clearbuff(u32 w0, u32 w1) {
    u32 addr = w0 & 0xFFFFFF;
    u32 count = w1 & 0xFFFF;

    if (addr + count > DMEM_SIZE) {
        count = DMEM_SIZE - addr;
    }
    memset(&rspa.dmem[addr], 0, count);
}

static void op_dmemmove(u32 w0, u32 w1) {
    u32 src = w0 & 0xFFFFFF;
    u32 dst = (w1 >> 16) & 0xFFFF;
    u32 count = w1 & 0xFFFF;

    if (src + count > DMEM_SIZE || dst + count > DMEM_SIZE) {
        return;
    }
    shz_memmove(&rspa.dmem[dst], &rspa.dmem[src], count); // may overlap
}

static void op_loadbuff(u32 w1) {
    u32 count = rspa.count;

    if (rspa.in + count > DMEM_SIZE) {
        count = DMEM_SIZE - rspa.in;
    }
    // Raw bytes. ADPCM frames stay a byte stream; RAW16 was swapped in the asset
    // image already. See the header comment.
    shz_memcpy(&rspa.dmem[rspa.in], rdram(w1), count);
}

static void op_savebuff(u32 w1) {
    u32 count = rspa.count;

    if (rspa.out + count > DMEM_SIZE) {
        count = DMEM_SIZE - rspa.out;
    }
    shz_memcpy(rdram(w1), &rspa.dmem[rspa.out], count);
}

static void op_loadadpcm(u32 w0, u32 w1) {
    // aLoadADPCM(pkt, c, d): count in w0 (24 bits), DRAM address in w1. Count is in
    // BYTES; the table is s16 entries. Doubles as the POLEF coefficient load
    // (alFilterNew does aLoadADPCM(32, fccoef)).
    u32 count = w0 & 0xFFFFFF;
    u32 entries = count >> 1;
    const s16 *src = rdram(w1);
    u32 i;

    if (entries > 512) {
        entries = 512;
    }

    shz_memcpy(rspa.table, src, entries * sizeof(rspa.table[0]));

    {
        u32 fEntries = entries;
        float *dstF = &rspa.tableF[0][0][0];

        if (fEntries > (sizeof(rspa.tableF) / sizeof(float))) {
            fEntries = sizeof(rspa.tableF) / sizeof(float);
        }
        for (i = 0; i < fEntries; i++) {
            dstF[i] = (float) rspa.table[i] * (1.0f / 2048.0f);
        }
    }
}

SHZ_NO_UNROLL_LOOPS
static void op_mix(u32 w0, u32 w1) {
    u16 dmemi = (w1 >> 16) & 0xFFFF;
    s16 *src = dmem16(dmemi);
    SHZ_PREFETCH(src);

    s16 gain = (s16) (w0 & 0xFFFF);
    u16 dmemo = w1 & 0xFFFF;
    s16 *dst = dmem16(dmemo);
    s32 n = rspa.count >> 1; // bytes -> samples
    float gainF = (float)gain * (1.0f / 32768.0f);

    for (int k = 0; k < n; k += 8) {
        SHZ_PREFETCH(dst);
        float m0 = (float)src[0] * gainF;
        float m1 = (float)src[1] * gainF;
        float m2 = (float)src[2] * gainF;
        float m3 = (float)src[3] * gainF;
        float m4 = (float)src[4] * gainF;
        float m5 = (float)src[5] * gainF;
        float m6 = (float)src[6] * gainF;
        float m7 = (float)src[7] * gainF;
        src += 8;

        m0 += (float)dst[0];
        m1 += (float)dst[1];
        m2 += (float)dst[2];
        m3 += (float)dst[3];
        m4 += (float)dst[4];
        m5 += (float)dst[5];
        m6 += (float)dst[6];
        m7 += (float)dst[7];

        SHZ_PREFETCH(src);
        dst[0] = clamp16f(m0);
        dst[1] = clamp16f(m1);
        dst[2] = clamp16f(m2);
        dst[3] = clamp16f(m3);
        dst[4] = clamp16f(m4);
        dst[5] = clamp16f(m5);
        dst[6] = clamp16f(m6);
        dst[7] = clamp16f(m7);
        dst += 8;
    }
}

// We no longer interleave shit, cuz the AICA would just
// make us deinterleave it.
static void op_interleave(u32 w1) {
    u16 rightAddr = w1 & 0xFFFF;
    u16 leftAddr = (w1 >> 16) & 0xFFFF;
    const s16 *r = dmem16(rightAddr);
    const s16 *l = dmem16(leftAddr);
    s16 *dst = dmem16(rspa.out);
    s32 n = rspa.count >> 1; // samples per channel, this chunk only

    shz_memcpy(dst, l, n * sizeof(s16));
    shz_memcpy(dst + n, r, n * sizeof(s16));
}

alignas(32) static const float sResampleTable[64][4] = {
    {3129.0f, 26285.0f, 3398.0f, -33.0f}, {2873.0f, 26262.0f, 3679.0f, -40.0f},
    {2628.0f, 26217.0f, 3971.0f, -48.0f}, {2394.0f, 26150.0f, 4276.0f, -56.0f},
    {2173.0f, 26061.0f, 4592.0f, -65.0f}, {1963.0f, 25950.0f, 4920.0f, -74.0f},
    {1764.0f, 25817.0f, 5260.0f, -84.0f}, {1576.0f, 25663.0f, 5611.0f, -95.0f},
    {1399.0f, 25487.0f, 5974.0f, -106.0f}, {1233.0f, 25291.0f, 6347.0f, -118.0f},
    {1077.0f, 25075.0f, 6732.0f, -130.0f}, {932.0f, 24838.0f, 7127.0f, -143.0f},
    {796.0f, 24583.0f, 7532.0f, -156.0f}, {671.0f, 24309.0f, 7947.0f, -170.0f},
    {554.0f, 24016.0f, 8371.0f, -184.0f}, {446.0f, 23706.0f, 8804.0f, -198.0f},
    {347.0f, 23379.0f, 9246.0f, -212.0f}, {257.0f, 23036.0f, 9696.0f, -226.0f},
    {174.0f, 22678.0f, 10153.0f, -240.0f}, {99.0f, 22304.0f, 10618.0f, -254.0f},
    {31.0f, 21917.0f, 11088.0f, -268.0f}, {-30.0f, 21517.0f, 11564.0f, -280.0f},
    {-84.0f, 21104.0f, 12045.0f, -293.0f}, {-132.0f, 20679.0f, 12531.0f, -304.0f},
    {-173.0f, 20244.0f, 13020.0f, -314.0f}, {-210.0f, 19799.0f, 13512.0f, -323.0f},
    {-241.0f, 19345.0f, 14006.0f, -330.0f}, {-267.0f, 18882.0f, 14501.0f, -336.0f},
    {-289.0f, 18413.0f, 14997.0f, -340.0f}, {-306.0f, 17937.0f, 15493.0f, -341.0f},
    {-320.0f, 17456.0f, 15988.0f, -340.0f}, {-330.0f, 16970.0f, 16480.0f, -337.0f},
    {-337.0f, 16480.0f, 16970.0f, -330.0f}, {-340.0f, 15988.0f, 17456.0f, -320.0f},
    {-341.0f, 15493.0f, 17937.0f, -306.0f}, {-340.0f, 14997.0f, 18413.0f, -289.0f},
    {-336.0f, 14501.0f, 18882.0f, -267.0f}, {-330.0f, 14006.0f, 19345.0f, -241.0f},
    {-323.0f, 13512.0f, 19799.0f, -210.0f}, {-314.0f, 13020.0f, 20244.0f, -173.0f},
    {-304.0f, 12531.0f, 20679.0f, -132.0f}, {-293.0f, 12045.0f, 21104.0f, -84.0f},
    {-280.0f, 11564.0f, 21517.0f, -30.0f}, {-268.0f, 11088.0f, 21917.0f, 31.0f},
    {-254.0f, 10618.0f, 22304.0f, 99.0f}, {-240.0f, 10153.0f, 22678.0f, 174.0f},
    {-226.0f, 9696.0f, 23036.0f, 257.0f}, {-212.0f, 9246.0f, 23379.0f, 347.0f},
    {-198.0f, 8804.0f, 23706.0f, 446.0f}, {-184.0f, 8371.0f, 24016.0f, 554.0f},
    {-170.0f, 7947.0f, 24309.0f, 671.0f}, {-156.0f, 7532.0f, 24583.0f, 796.0f},
    {-143.0f, 7127.0f, 24838.0f, 932.0f}, {-130.0f, 6732.0f, 25075.0f, 1077.0f},
    {-118.0f, 6347.0f, 25291.0f, 1233.0f}, {-106.0f, 5974.0f, 25487.0f, 1399.0f},
    {-95.0f, 5611.0f, 25663.0f, 1576.0f}, {-84.0f, 5260.0f, 25817.0f, 1764.0f},
    {-74.0f, 4920.0f, 25950.0f, 1963.0f}, {-65.0f, 4592.0f, 26061.0f, 2173.0f},
    {-56.0f, 4276.0f, 26150.0f, 2394.0f}, {-48.0f, 3971.0f, 26217.0f, 2628.0f},
    {-40.0f, 3679.0f, 26262.0f, 2873.0f}, {-33.0f, 3398.0f, 26285.0f, 3129.0f},
};

// ---------------------------------------------------------------------------
// A_RESAMPLE — pitch shift, 4-tap polyphase (ucode-accurate).
//
// The phase accumulator is 16.16: `pitch` from the command is a u16 that steps by
// pitch*2 per output sample. The low 16 bits select one of 64 filter phases
// (accu >> 10); the integer part advances the input window.
//
// State (16 s16 in RDRAM):
//   [0..3] the 4 input samples at the window position — the filter needs 4 taps of
//          history, and they must survive across command lists or every frame
//          boundary is a discontinuity (i.e. a click)
//   [4]    the fractional phase
// ---------------------------------------------------------------------------
static void op_resample(u32 w0, u32 w1) {
    const s16 *src = dmem16(rspa.in);
    SHZ_PREFETCH(src);

    u8 flags = (w0 >> 16) & 0xFF;
    u32 pitch = (w0 & 0xFFFF) << 1; // 16.16 step per output sample
    s16 *state = rdram(w1);
    s32 n = rspa.count >> 1; // output samples wanted
    u32 accu, accu_shift;
    s16 hist[4];
    s32 pos = 0; // window start in the virtual stream (hist, then src)
    s32 k;

    if (flags & A_INIT) {
        shz_memset2_16(hist, 0);
        accu = 0;
    } else {
        shz_memcpy2_16(hist, &state[0]);
        accu = (u16) state[4];
    }

    s16 *dst = dmem16(rspa.out);
    SHZ_PREFETCH(dst);

#define RESAMPLE_TAP(i) (((i) < 4) ? hist[i] : src[(i) - 4])

    for (k = 0; k < n && pos < 4; k++) {
        const float *tbl = sResampleTable[accu >> 10]; // 64 phases
       // SHZ_PREFETCH(tbl);

        accu += pitch;
        pos += accu >> 16;
        accu &= 0xFFFF;

        float sample = shz_dot8f((float) RESAMPLE_TAP(pos + 0), (float) RESAMPLE_TAP(pos + 1),
                                  (float) RESAMPLE_TAP(pos + 2), (float) RESAMPLE_TAP(pos + 3),
                                  tbl[0], tbl[1], tbl[2], tbl[3])
                        * (1.0f / 32768.0f);

        dst[k] = clamp16f(sample);
    }


    SHZ_PREFETCH(dst);

    for (; k < n; k++) {
        const s16 *tap = &src[pos - 4];
        //SHZ_PREFETCH(tap);
        const float *tbl = sResampleTable[accu >> 10]; // 64 phases
        accu += pitch;
        pos += accu >> 16;
        accu &= 0xFFFF;

        float sample = shz_dot8f((float) tap[0], (float) tap[1], (float) tap[2], (float) tap[3],
                                  tbl[0], tbl[1], tbl[2], tbl[3])
                        * (1.0f / 32768.0f);

        dst[k] = clamp16f(sample);
    }

    // Carry the window and the phase into the next command list.
    for (k = 0; k < 4; k++) {
        state[k] = RESAMPLE_TAP(pos + k);
    }
    state[4] = (s16) (u16) accu;

#undef RESAMPLE_TAP
}

static const float sNybblesF[256][2] __attribute__((aligned(32))) = {
    { 0.0f, 0.0f },   { 0.0f, 1.0f },   { 0.0f, 2.0f },   { 0.0f, 3.0f },   { 0.0f, 4.0f },   { 0.0f, 5.0f },
    { 0.0f, 6.0f },   { 0.0f, 7.0f },   { 0.0f, -8.0f },  { 0.0f, -7.0f },  { 0.0f, -6.0f },  { 0.0f, -5.0f },
    { 0.0f, -4.0f },  { 0.0f, -3.0f },  { 0.0f, -2.0f },  { 0.0f, -1.0f },  { 1.0f, 0.0f },   { 1.0f, 1.0f },
    { 1.0f, 2.0f },   { 1.0f, 3.0f },   { 1.0f, 4.0f },   { 1.0f, 5.0f },   { 1.0f, 6.0f },   { 1.0f, 7.0f },
    { 1.0f, -8.0f },  { 1.0f, -7.0f },  { 1.0f, -6.0f },  { 1.0f, -5.0f },  { 1.0f, -4.0f },  { 1.0f, -3.0f },
    { 1.0f, -2.0f },  { 1.0f, -1.0f },  { 2.0f, 0.0f },   { 2.0f, 1.0f },   { 2.0f, 2.0f },   { 2.0f, 3.0f },
    { 2.0f, 4.0f },   { 2.0f, 5.0f },   { 2.0f, 6.0f },   { 2.0f, 7.0f },   { 2.0f, -8.0f },  { 2.0f, -7.0f },
    { 2.0f, -6.0f },  { 2.0f, -5.0f },  { 2.0f, -4.0f },  { 2.0f, -3.0f },  { 2.0f, -2.0f },  { 2.0f, -1.0f },
    { 3.0f, 0.0f },   { 3.0f, 1.0f },   { 3.0f, 2.0f },   { 3.0f, 3.0f },   { 3.0f, 4.0f },   { 3.0f, 5.0f },
    { 3.0f, 6.0f },   { 3.0f, 7.0f },   { 3.0f, -8.0f },  { 3.0f, -7.0f },  { 3.0f, -6.0f },  { 3.0f, -5.0f },
    { 3.0f, -4.0f },  { 3.0f, -3.0f },  { 3.0f, -2.0f },  { 3.0f, -1.0f },  { 4.0f, 0.0f },   { 4.0f, 1.0f },
    { 4.0f, 2.0f },   { 4.0f, 3.0f },   { 4.0f, 4.0f },   { 4.0f, 5.0f },   { 4.0f, 6.0f },   { 4.0f, 7.0f },
    { 4.0f, -8.0f },  { 4.0f, -7.0f },  { 4.0f, -6.0f },  { 4.0f, -5.0f },  { 4.0f, -4.0f },  { 4.0f, -3.0f },
    { 4.0f, -2.0f },  { 4.0f, -1.0f },  { 5.0f, 0.0f },   { 5.0f, 1.0f },   { 5.0f, 2.0f },   { 5.0f, 3.0f },
    { 5.0f, 4.0f },   { 5.0f, 5.0f },   { 5.0f, 6.0f },   { 5.0f, 7.0f },   { 5.0f, -8.0f },  { 5.0f, -7.0f },
    { 5.0f, -6.0f },  { 5.0f, -5.0f },  { 5.0f, -4.0f },  { 5.0f, -3.0f },  { 5.0f, -2.0f },  { 5.0f, -1.0f },
    { 6.0f, 0.0f },   { 6.0f, 1.0f },   { 6.0f, 2.0f },   { 6.0f, 3.0f },   { 6.0f, 4.0f },   { 6.0f, 5.0f },
    { 6.0f, 6.0f },   { 6.0f, 7.0f },   { 6.0f, -8.0f },  { 6.0f, -7.0f },  { 6.0f, -6.0f },  { 6.0f, -5.0f },
    { 6.0f, -4.0f },  { 6.0f, -3.0f },  { 6.0f, -2.0f },  { 6.0f, -1.0f },  { 7.0f, 0.0f },   { 7.0f, 1.0f },
    { 7.0f, 2.0f },   { 7.0f, 3.0f },   { 7.0f, 4.0f },   { 7.0f, 5.0f },   { 7.0f, 6.0f },   { 7.0f, 7.0f },
    { 7.0f, -8.0f },  { 7.0f, -7.0f },  { 7.0f, -6.0f },  { 7.0f, -5.0f },  { 7.0f, -4.0f },  { 7.0f, -3.0f },
    { 7.0f, -2.0f },  { 7.0f, -1.0f },  { -8.0f, 0.0f },  { -8.0f, 1.0f },  { -8.0f, 2.0f },  { -8.0f, 3.0f },
    { -8.0f, 4.0f },  { -8.0f, 5.0f },  { -8.0f, 6.0f },  { -8.0f, 7.0f },  { -8.0f, -8.0f }, { -8.0f, -7.0f },
    { -8.0f, -6.0f }, { -8.0f, -5.0f }, { -8.0f, -4.0f }, { -8.0f, -3.0f }, { -8.0f, -2.0f }, { -8.0f, -1.0f },
    { -7.0f, 0.0f },  { -7.0f, 1.0f },  { -7.0f, 2.0f },  { -7.0f, 3.0f },  { -7.0f, 4.0f },  { -7.0f, 5.0f },
    { -7.0f, 6.0f },  { -7.0f, 7.0f },  { -7.0f, -8.0f }, { -7.0f, -7.0f }, { -7.0f, -6.0f }, { -7.0f, -5.0f },
    { -7.0f, -4.0f }, { -7.0f, -3.0f }, { -7.0f, -2.0f }, { -7.0f, -1.0f }, { -6.0f, 0.0f },  { -6.0f, 1.0f },
    { -6.0f, 2.0f },  { -6.0f, 3.0f },  { -6.0f, 4.0f },  { -6.0f, 5.0f },  { -6.0f, 6.0f },  { -6.0f, 7.0f },
    { -6.0f, -8.0f }, { -6.0f, -7.0f }, { -6.0f, -6.0f }, { -6.0f, -5.0f }, { -6.0f, -4.0f }, { -6.0f, -3.0f },
    { -6.0f, -2.0f }, { -6.0f, -1.0f }, { -5.0f, 0.0f },  { -5.0f, 1.0f },  { -5.0f, 2.0f },  { -5.0f, 3.0f },
    { -5.0f, 4.0f },  { -5.0f, 5.0f },  { -5.0f, 6.0f },  { -5.0f, 7.0f },  { -5.0f, -8.0f }, { -5.0f, -7.0f },
    { -5.0f, -6.0f }, { -5.0f, -5.0f }, { -5.0f, -4.0f }, { -5.0f, -3.0f }, { -5.0f, -2.0f }, { -5.0f, -1.0f },
    { -4.0f, 0.0f },  { -4.0f, 1.0f },  { -4.0f, 2.0f },  { -4.0f, 3.0f },  { -4.0f, 4.0f },  { -4.0f, 5.0f },
    { -4.0f, 6.0f },  { -4.0f, 7.0f },  { -4.0f, -8.0f }, { -4.0f, -7.0f }, { -4.0f, -6.0f }, { -4.0f, -5.0f },
    { -4.0f, -4.0f }, { -4.0f, -3.0f }, { -4.0f, -2.0f }, { -4.0f, -1.0f }, { -3.0f, 0.0f },  { -3.0f, 1.0f },
    { -3.0f, 2.0f },  { -3.0f, 3.0f },  { -3.0f, 4.0f },  { -3.0f, 5.0f },  { -3.0f, 6.0f },  { -3.0f, 7.0f },
    { -3.0f, -8.0f }, { -3.0f, -7.0f }, { -3.0f, -6.0f }, { -3.0f, -5.0f }, { -3.0f, -4.0f }, { -3.0f, -3.0f },
    { -3.0f, -2.0f }, { -3.0f, -1.0f }, { -2.0f, 0.0f },  { -2.0f, 1.0f },  { -2.0f, 2.0f },  { -2.0f, 3.0f },
    { -2.0f, 4.0f },  { -2.0f, 5.0f },  { -2.0f, 6.0f },  { -2.0f, 7.0f },  { -2.0f, -8.0f }, { -2.0f, -7.0f },
    { -2.0f, -6.0f }, { -2.0f, -5.0f }, { -2.0f, -4.0f }, { -2.0f, -3.0f }, { -2.0f, -2.0f }, { -2.0f, -1.0f },
    { -1.0f, 0.0f },  { -1.0f, 1.0f },  { -1.0f, 2.0f },  { -1.0f, 3.0f },  { -1.0f, 4.0f },  { -1.0f, 5.0f },
    { -1.0f, 6.0f },  { -1.0f, 7.0f },  { -1.0f, -8.0f }, { -1.0f, -7.0f }, { -1.0f, -6.0f }, { -1.0f, -5.0f },
    { -1.0f, -4.0f }, { -1.0f, -3.0f }, { -1.0f, -2.0f }, { -1.0f, -1.0f }
};

static const float sShiftF[16] __attribute__((aligned(32))) = {
    1.0f,    2.0f,    4.0f,     8.0f,    16.0f,   32.0f,   64.0f,   128.0f,
    256.0f,  512.0f,  1024.0f,  2048.0f, 4096.0f, 8192.0f, 16384.0f, 32768.0f
};

// ---------------------------------------------------------------------------
// A_ADPCM — Nintendo 4-bit ADPCM, 16 samples per 9-byte frame.
//
// Frame: 1 header byte (scale in the high nibble, predictor index in the low), then
// 8 bytes of 16 packed 4-bit nibbles.
//
// State is ADPCM_STATE (16 s16) = the last 16 samples decoded, which is also the
// layout of ALADPCMloop.state in the bank (the 16 samples preceding a loop point) —
// so A_LOOP can load it straight from there. That shared layout is why this is a
// fixed format and not ours to choose.
// ---------------------------------------------------------------------------
static void op_adpcm(u32 w0, u32 w1) {
    s16 *dst = dmem16(rspa.out);
    SHZ_PREFETCH(dst);

    u8 flags = (w0 >> 16) & 0xFF;
    s16 *state = rdram(w1);

    if (flags & A_INIT)
        shz_memset2_16(dst, 0);
    else if (flags & A_LOOP)
        shz_memcpy2_16(dst, rdram(rspa.loopAddr)); // loop-point context, from the bank
    else
        shz_memcpy2_16(dst, state);

    const u8 *src = &rspa.dmem[rspa.in];
    SHZ_PREFETCH(src);

    s32 outSamples = rspa.count >> 1;
    dst += 16;

    {
        float l1 = (float)dst[-1];
        float l2 = (float)dst[-2];

#pragma GCC unroll 1
        while(outSamples > 0) {
            const u8 header = *src++;
            const s32 scale = header >> 4;
            const s32 predIdx = header & 0xF;
            const float shift = sShiftF[scale];
            float residual[16];

            SHZ_PREFETCH(sNybblesF[*src]);

            // order 2 => 8 taps per predictor per book.
            const float *book1 = rspa.tableF[predIdx][0];
            const float *book2 = rspa.tableF[predIdx][1];

#pragma GCC unroll 1
            for(int i = 0; i < 3; i++) {
                const u8 byte0 = src[i];
                const u8 byte1 = src[4 + i];

                SHZ_PREFETCH(sNybblesF[byte1]);
                residual[i * 2 + 0] = sNybblesF[byte0][0] * shift;
                residual[i * 2 + 1] = sNybblesF[byte0][1] * shift;

                SHZ_PREFETCH(sNybblesF[src[i + 1]]);
                residual[8 + i * 2 + 0] = sNybblesF[byte1][0] * shift;
                residual[8 + i * 2 + 1] = sNybblesF[byte1][1] * shift;
            }
            {
                const u8 byte0 = src[3];
                const u8 byte1 = src[4 + 3];

                SHZ_PREFETCH(sNybblesF[byte1]);
                residual[3 * 2 + 0] = sNybblesF[byte0][0] * shift;
                residual[3 * 2 + 1] = sNybblesF[byte0][1] * shift;

                SHZ_PREFETCH(book1);
                residual[8 + 3 * 2 + 0] = sNybblesF[byte1][0] * shift;
                residual[8 + 3 * 2 + 1] = sNybblesF[byte1][1] * shift;
            }
            src += 8;

            shz_xmtrx_load_cols_4x4((const SHZ_ALIASING shz_vec4_t*)book1,
                                    (const SHZ_ALIASING shz_vec4_t*)book2,
                                    (const SHZ_ALIASING shz_vec4_t*)&book1[4],
                                    (const SHZ_ALIASING shz_vec4_t*)&book2[4]);

#pragma GCC unroll 1
            for(int half = 0; half < 2; half++) {
                shz_vec4_t acc[2];
                SHZ_ALIASING float *accf = (SHZ_ALIASING float *)acc;
                const float *res = &residual[half * 8];

                acc [0]  = shz_xmtrx_transform_vec4(shz_vec4_init(l2, l1, 0.0f, 0.0f));
                accf[0] += res[0];
                accf[1] += res[1];
                accf[2] += res[2];
                accf[3] += res[3];

                acc [1]  = shz_xmtrx_transform_vec4(shz_vec4_init(0.0f, 0.0f, l2, l1));
                accf[4] += res[4];
                accf[5] += res[5];
                accf[6] += res[6];
                accf[7] += res[7];

                {
                    register float fr0 asm("fr0") = 1.0f;
                    register float fr1 asm("fr1") = res[0];
                    register float fr2 asm("fr2") = res[1];
                    register float fr3 asm("fr3") = res[2];

                    register float fr4 asm("fr4") = accf[2];
                    register float fr5 asm("fr5") = book2[1];
                    register float fr6 asm("fr6") = book2[0];
                    register float fr7 asm("fr7") = 0.0f;

                    register float fr8  asm("fr8");
                    register float fr9  asm("fr9");
                    register float fr10 asm("fr10");
                    register float fr11 asm("fr11");

                    fr8  = accf[7];
                    fr9  = book2[6];
                    fr10 = book2[5];
                    fr11 = book2[4];

                    asm volatile("fipr fv0, fv4"
                        : "+f" (fr7)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr4), "f" (fr5), "f" (fr6));

                    fr4 = accf[3];
                    fr5 = book2[2];
                    fr6 = book2[1];

                    asm volatile("fipr fv0, fv8"
                        : "+f" (fr11)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr8), "f" (fr9), "f" (fr10));

                    accf[2] = fr7;
                    fr7 = book2[0];
                    fr8 = accf[4];
                    fr9 = book2[3];
                    fr10 = book2[2];

                    asm volatile("fipr fv0, fv4\n"
                        : "+f" (fr7)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr4), "f" (fr5), "f" (fr6));

                    accf[7] = fr11;
                    fr11 = book2[1];
                    fr4 = accf[5];
                    fr5 = book2[4];
                    fr6 = book2[3];

                    asm volatile("fipr fv0, fv8"
                        : "+f" (fr11)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr8), "f" (fr9), "f" (fr10));

                    accf[3] = fr7;
                    fr7 = book2[2];
                    fr8 = accf[6];
                    fr9 = book2[5];
                    fr10 = book2[4];

                    asm volatile("fipr fv0, fv4\n"
                        : "+f" (fr7)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr4), "f" (fr5), "f" (fr6));

                    accf[4] = fr11;
                    fr11 = book2[3];
                    fr4 = res[3];
                    fr5 = res[4];
                    fr6 = res[5];

                    asm volatile("fipr fv0, fv8"
                        : "+f" (fr11)
                        : "f" (fr0), "f" (fr1), "f" (fr2), "f" (fr3),
                          "f" (fr8), "f" (fr9), "f" (fr10));

                    accf[5] = fr7;
                    fr7 = res[6];
                    fr8 = book2[3];
                    fr9 = book2[2];
                    fr10 = book2[1];
                    accf[6] = fr11;
                    fr11 = book2[0];
                    fr0 = book2[2];

                    asm volatile("fipr fv4, fv8"
                        : "+f" (fr11)
                        : "f" (fr4), "f" (fr5), "f" (fr6), "f" (fr7),
                          "f" (fr8), "f" (fr9), "f" (fr10));

                    fr1 = book2[1];
                    fr2 = book2[0];
                    fr3 = 0.0f;

                    asm volatile("fipr fv4, fv0"
                        : "+f" (fr3)
                        : "f" (fr4), "f" (fr5), "f" (fr6), "f" (fr7),
                          "f" (fr0), "f" (fr1), "f" (fr2));

                    accf[7] += fr11;
                    accf[6] += fr3;
                }

                SHZ_PREFETCH(dst);

                accf[1] += book2[0] * res[0];
                accf[5] += (book2[1] * res[3]) + (book2[0] * res[4]);
                accf[4] += (book2[0] * res[3]);

                *dst++ = clamp16f(accf[0]);
                *dst++ = clamp16f(accf[1]);
                *dst++ = clamp16f(accf[2]);
                *dst++ = clamp16f(accf[3]);
                *dst++ = clamp16f(accf[4]);
                *dst++ = clamp16f(accf[5]);

                SHZ_PREFETCH(src);

                l2 = (float)clamp16f(accf[6]);
                *dst++ = l2;
                l1 = (float)clamp16f(accf[7]);
                *dst++ = l1;
            }

            outSamples -= 16;
        }
    }

    shz_memcpy2_16(state, dst - 16); // carry into the next command list
}

// ---------------------------------------------------------------------------
// A_ENVMIXER — the volume/pan/envelope stage, and the only opcode with four
// outputs: dry L/R (the main bus) and wet L/R (the reverb bus).
//
// State (40 bytes in RDRAM, layout ours): the ENTIRE parameter set, not just the
// volume accumulators. alEnvmixerPull only emits the aSetVolume latches on a
// voice's A_INIT frame (or after a param event resets e->first); every steady
// frame is a bare aEnvMixer(A_CONTINUE) that expects targets, rates and dry/wet
// to come back out of this state block. Reading the global latches on CONTINUE
// instead means "whatever voice's SETVOLs ran last" — cross-voice envelope, pan
// and reverb-send contamination on every continuing voice.
//
//   [0..1] volAccu L (16.16)   [2..3] volAccu R
//   [4]    target L            [5]    target R
//   [6..7] rate L (16.16)      [8..9] rate R
//   [10]   dry                 [11]   wet
// ---------------------------------------------------------------------------
SHZ_FORCE_INLINE
void ramp_update(s32 *volAccu, const s32 *target, const s32 *rate) {
    s32 i;

    for (i = 0; i < 2; i++) {
        if (rate[i] == 0) {
            continue; // steady volume: hold it, don't snap to the target
        }
        volAccu[i] += rate[i] >> 3;
        if (rate[i] > 0 ? (volAccu[i] >> 16) > target[i] : (volAccu[i] >> 16) < target[i]) {
            volAccu[i] = target[i] << 16;
        }
    }
}

static void op_envmixer(u32 w0, u32 w1) {
    u8 flags = (w0 >> 16) & 0xFF;
    s16 *state = rdram(w1);
    const s16 *src = dmem16(rspa.in);
    s16 *dryL = dmem16(rspa.out);
    s16 *dryR = dmem16(rspa.dryRight);
    s16 *wetL = dmem16(rspa.wetLeft);
    s16 *wetR = dmem16(rspa.wetRight);
    s32 n = rspa.count >> 1;
    s32 volAccu[2];
    s32 target[2], rate[2], dry, wet;

    if (flags & A_INIT) {
        volAccu[0] = (s32) rspa.vol[0] << 16;
        volAccu[1] = (s32) rspa.vol[1] << 16;
        target[0] = rspa.target[0];
        target[1] = rspa.target[1];
        rate[0] = rspa.rate[0];
        rate[1] = rspa.rate[1];
        dry = rspa.dry;
        wet = rspa.wet;
    } else {
        volAccu[0] = ((s32) (u16) state[0] << 16) | (u16) state[1];
        volAccu[1] = ((s32) (u16) state[2] << 16) | (u16) state[3];
        target[0] = state[4];
        target[1] = state[5];
        rate[0] = (s32) (((u32) (u16) state[6] << 16) | (u16) state[7]);
        rate[1] = (s32) (((u32) (u16) state[8] << 16) | (u16) state[9]);
        dry = state[10];
        wet = state[11];
    }

    SHZ_PREFETCH(src);

    if (rate[0] == 0 && rate[1] == 0) {
        const s32 vl = volAccu[0] >> 16;
        const s32 vr = volAccu[1] >> 16;

        if (flags & A_AUX) {
#pragma GCC unroll 1
            for (int k = 0; k < (n >> 1); ++k) {
                SHZ_PREFETCH(dryL);
                s32 s1 = *src++;
                s32 s2 = *src++;

                s32 l1 = (s1 * vl) >> 15;
                s32 l2 = (s2 * vl) >> 15;

                s32 r1 = (s1 * vr) >> 15;
                s32 r2 = (s2 * vr) >> 15;

                SHZ_PREFETCH(dryR);
                dryL[0] = clamp16(dryL[0] + ((l1 * dry) >> 15));
                dryL[1] = clamp16(dryL[1] + ((l2 * dry) >> 15));
                dryL += 2;

                SHZ_PREFETCH(wetL);
                dryR[0] = clamp16(dryR[0] + ((r1 * dry) >> 15));
                dryR[1] = clamp16(dryR[1] + ((r2 * dry) >> 15));
                dryR += 2;

                SHZ_PREFETCH(wetR);
                wetL[0] = clamp16(wetL[0] + ((l1 * wet) >> 15));
                wetL[1] = clamp16(wetL[1] + ((l2 * wet) >> 15));
                wetL += 2;

                SHZ_PREFETCH(src);
                wetR[0] = clamp16(wetR[0] + ((r1 * wet) >> 15));
                wetR[1] = clamp16(wetR[1] + ((r2 * wet) >> 15));
                wetR += 2;
            }
        } else {
#pragma GCC unroll 1
            for (int k = 0; k < (n >> 1); ++k) {
                SHZ_PREFETCH(dryL);
                s32 s1 = *src++;
                s32 s2 = *src++;

                s32 l1 = (s1 * vl) >> 15;
                s32 l2 = (s2 * vl) >> 15;

                s32 r1 = (s1 * vr) >> 15;
                s32 r2 = (s2 * vr) >> 15;

                SHZ_PREFETCH(dryR);
                dryL[0] = clamp16(dryL[0] + ((l1 * dry) >> 15));
                dryL[1] = clamp16(dryL[1] + ((l2 * dry) >> 15));
                dryL += 2;

                SHZ_PREFETCH(src);
                dryR[0] = clamp16(dryR[0] + ((r1 * dry) >> 15));
                dryR[1] = clamp16(dryR[1] + ((r2 * dry) >> 15));
                dryR += 2;
            }
        }
    } else if (flags & A_AUX) {
#pragma GCC unroll 2
        for (int k = 0; k < n; k++) {
            s32 s = src[k];
            s32 vl = volAccu[0] >> 16;
            s32 vr = volAccu[1] >> 16;
            s32 l = (s * vl) >> 15;
            s32 r = (s * vr) >> 15;

            dryL[k] = clamp16(dryL[k] + ((l * dry) >> 15));
            dryR[k] = clamp16(dryR[k] + ((r * dry) >> 15));
            wetL[k] = clamp16(wetL[k] + ((l * wet) >> 15));
            wetR[k] = clamp16(wetR[k] + ((r * wet) >> 15));

            ramp_update(volAccu, target, rate);
        }
    } else {
#pragma GCC unroll 2
        for (int k = 0; k < n; k++) {
            s32 s = src[k];
            s32 vl = volAccu[0] >> 16;
            s32 vr = volAccu[1] >> 16;
            s32 l = (s * vl) >> 15;
            s32 r = (s * vr) >> 15;

            dryL[k] = clamp16(dryL[k] + ((l * dry) >> 15));
            dryR[k] = clamp16(dryR[k] + ((r * dry) >> 15));

            ramp_update(volAccu, target, rate);
        }
    }

    state[0] = (s16) (u16) (volAccu[0] >> 16);
    state[1] = (s16) (u16) (volAccu[0] & 0xFFFF);
    state[2] = (s16) (u16) (volAccu[1] >> 16);
    state[3] = (s16) (u16) (volAccu[1] & 0xFFFF);
    state[4] = (s16) target[0];
    state[5] = (s16) target[1];
    state[6] = (s16) (u16) ((u32) rate[0] >> 16);
    state[7] = (s16) (u16) ((u32) rate[0] & 0xFFFF);
    state[8] = (s16) (u16) ((u32) rate[1] >> 16);
    state[9] = (s16) (u16) ((u32) rate[1] & 0xFFFF);
    state[10] = (s16) dry;
    state[11] = (s16) wet;
}

// A_POLEF — the reverb bus's lowpass. Coefficients come from the table loaded by
// the LOADADPCM immediately before it (_filterBuffer: aLoadADPCM(32, fccoef)).
//
// The coefficient vector _init_lpfilter builds is fccoef[0..7] = 0, fccoef[8] = fc
// and fccoef[9..15] = fc², fc³, … — the powers exist so the ucode can run its
// 8-samples-at-a-time ADPCM predictor over the block, but the filter they encode
// is exactly the one-pole recursion y[n] = gain·x[n] + fc·y[n-1]. Implement that
// directly. Everything is Q14 (SCALE = 16384 in drvrnew.c; fgain = SCALE - fc, so
// DC gain is unity).
SHZ_COLD
static void op_polef(u32 w0, u32 w1) {
    s16 *src = dmem16(rspa.in);
    SHZ_PREFETCH(src);

    s16*    dst = dmem16(rspa.out);
    s16*  state = rdram(w1);
    u8    flags = (w0 >> 16) & 0xFF;
    float  gain = (s16) (w0 & 0xFFFF);
    float    fc = rspa.table[8];
    int       n = rspa.count >> 3;
    float    y0 = (flags & A_INIT)? 0.0f : state[0];

    const float      A  = gain * (1.0f / 16384.0f);
    const float      B  =   fc * (1.0f / 16384.0f);
    const shz_vec4_t BV = shz_vec4_init(B, (B * B   ), (B * B * B), (B * B * B * B));
    const shz_vec4_t AV = shz_vec4_init(A, (A * BV.x), (A * BV.y ), (A * BV.z     ));

    shz_xmtrx_init_lower_triangular(AV, AV.xyz, AV.xy, AV.x);

    for(int k = 0; k < n; ++k) {
        SHZ_PREFETCH(dst);

        const shz_vec4_t in   = shz_vec4_init(src[0], src[1], src[2], src[3]);
        const shz_vec4_t part = shz_xmtrx_transform_vec4(in);
              shz_vec4_t out  = shz_vec4_add(part, shz_vec4_scale(BV, y0));

        SHZ_PREFETCH(src += 4);

        if(out.x < -32768.0f || out.x > 32767.0f) goto y1;
        *dst++ = (s16)out.x;

        if(out.y < -32768.0f || out.y > 32767.0f) goto y2;
        *dst++ = (s16)out.y;

        if(out.z < -32768.0f || out.z > 32767.0f) goto y3;
        *dst++ = (s16)out.z;

        if(out.w < -32768.0f || out.w > 32767.0f) goto y4;
        *dst++ = (s16)out.w;

        y0 = out.w;
        continue;

    y1: out.x = fclamp16f((in.x * gain + fc *    y0) * (1.0f / 16384.0f), dst++);
    y2: out.y = fclamp16f((in.y * gain + fc * out.x) * (1.0f / 16384.0f), dst++);
    y3: out.z = fclamp16f((in.z * gain + fc * out.y) * (1.0f / 16384.0f), dst++);
    y4: out.w = fclamp16f((in.w * gain + fc * out.z) * (1.0f / 16384.0f), dst++);
        y0    = out.w;
    }

    state[0] = (s16)y0;
    state[1] = 0;
}


// ---------------------------------------------------------------------------
// The interpreter
// ---------------------------------------------------------------------------
void pc_audio_hle_run(void *cmdList, s32 cmdLen, void *outBuf, s32 frameSamples) {
    const u32 *cmd = cmdList;
    s32 i;

    (void) outBuf;      // the list SAVEBUFFs into it itself
    (void) frameSamples;

    for (i = 0; i < cmdLen; i++) {
        u32 w0 = cmd[2 * i + 0];
        u32 w1 = cmd[2 * i + 1];
        u32 op = w0 >> 24;

        switch (op) {
            case A_SPNOOP:
                break;
            case A_SEGMENT:
                break; // segment 0, base 0 — addresses are already absolute
            case A_SETBUFF:
                op_setbuff(w0, w1);
                break;
            case A_SETVOL:
                op_setvol(w0, w1);
                break;
            case A_CLEARBUFF:
                op_clearbuff(w0, w1);
                break;
            case A_DMEMMOVE:
                op_dmemmove(w0, w1);
                break;
            case A_LOADBUFF:
                op_loadbuff(w1);
                break;
            case A_SAVEBUFF:
                op_savebuff(w1);
                break;
            case A_LOADADPCM:
                op_loadadpcm(w0, w1);
                break;
            case A_SETLOOP:
                rspa.loopAddr = w1;
                break;
            case A_ADPCM:
                op_adpcm(w0, w1);
                break;
            case A_RESAMPLE:
                op_resample(w0, w1);
                break;
            case A_ENVMIXER:
                op_envmixer(w0, w1);
                break;
            case A_MIXER:
                op_mix(w0, w1);
                break;
            case A_INTERLEAVE:
                op_interleave(w1);
                break;
            case A_POLEF:
                op_polef(w0, w1);
                break;
            default:
                break;
        }
    }
}
