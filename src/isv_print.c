#ifdef NON_MATCHING
#include "PR/ultratypes.h"
#include "stdarg.h"
#include "xstdio.h"
#include "string.h"

#define ISV_BASE     0xB3FF0000
#define ISV_MAGIC    0x49533634  /* "IS64" */
#define ISV_LEN_OFF  0x14
#define ISV_BUF_OFF  0x20
#define ISV_BUF_SIZE 0x200

static char sISVBuf[ISV_BUF_SIZE];
static u32  sISVLen;

static char *isv_prout(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && sISVLen < ISV_BUF_SIZE - 1; i++) {
        sISVBuf[sISVLen++] = src[i];
    }
    return (char *)1;
}

void isv_printf(const char *fmt, ...) {
    volatile u32 *base = (volatile u32 *)ISV_BASE;
    volatile u32 *buf  = (volatile u32 *)(ISV_BASE + ISV_BUF_OFF);
    u32 i, words;
    va_list args;

    /* write IS64 magic so ares recognises the device */
    base[0] = ISV_MAGIC;

    sISVLen = 0;
    va_start(args, fmt);
    _Printf(isv_prout, NULL, fmt, args);
    va_end(args);

    /* copy buffer as u32 words (IS-Viewer requires word-sized writes) */
    words = (sISVLen + 3) / 4;
    for (i = 0; i < words; i++) {
        u32 word = 0;
        u32 base_idx = i * 4;
        if (base_idx + 0 < sISVLen) word |= (u32)(u8)sISVBuf[base_idx + 0] << 24;
        if (base_idx + 1 < sISVLen) word |= (u32)(u8)sISVBuf[base_idx + 1] << 16;
        if (base_idx + 2 < sISVLen) word |= (u32)(u8)sISVBuf[base_idx + 2] << 8;
        if (base_idx + 3 < sISVLen) word |= (u32)(u8)sISVBuf[base_idx + 3];
        buf[i] = word;
    }

    /* commit length — this is what triggers ares to flush */
    base[ISV_LEN_OFF / 4] = sISVLen;
}
#endif /* NON_MATCHING */
