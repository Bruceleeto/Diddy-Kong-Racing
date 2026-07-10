#include "gzip.h"
#include "asset_loading.h"
#include "memory.h"
#include "PR/os_libc.h"

/************ .data ************/

huft *gHuftTable = NULL; // gzip_huft_alloc
s32 *gPackedHeader = NULL;
u8 *gzip_inflate_input = NULL;
u8 *gzip_inflate_output = NULL;

/*******************************/

/************ .rodata ************/

/* Tables for deflate from PKZIP's appnote.txt. */

/* Order of the bit length code lengths */
u8 gzip_border[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Copy lengths for literal codes 257..285 */
u16 gzip_cplens[31] = {
    3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0,  0
};

/* Extra bits for literal codes 257..285 (99 == invalid) */
u8 gzip_cplext[31] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 99, 99
};

/* Copy offsets for distance codes 0..29 */
u16 gzip_cpdist[30] = {
    1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

/* Extra bits for distance codes */
u8 gzip_cpdext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

u16 gzip_mask_bits[17] = {
    0x0000, 0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF,
    0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF
};

/*********************************/

/************ .bss ************/

u32 gzip_bit_buffer;
u32 gzip_num_bits;
s32 gHuftTablePos; // gzip_hufts

/******************************/

/**
 * Allocate space for the decompression heap and file header.
 */
void gzip_init(void) {
    gHuftTable = (huft *) mempool_alloc_safe(0x2800, COLOUR_TAG_BLACK);
    gPackedHeader = (s32 *) mempool_alloc_safe(0x10, COLOUR_TAG_BLACK);
}

/**
 * Converts a little endian value to big endian.
 * Official name: rzipUncompressSize
 * (so, this probably expects a gzip header)
 */
s32 byteswap32(u8 *arg0) {
    s32 value;
    value = *arg0++;
    value |= (*arg0++ << 8);
    value |= (*arg0++ << 16);
    value |= (*arg0 << 24);
    return value;
}

/**
 * Returns the uncompressed size of a gzip compressed asset.
 * Official name: rzipUncompressSizeROM
 */
s32 gzip_size_uncompressed(s32 assetIndex, s32 assetOffset) {
    asset_load(assetIndex, (u32) gPackedHeader, assetOffset, 8);
    return byteswap32((u8 *) gPackedHeader);
}

/**
 * Decompresses gzip data.
 * Returns the pointer to the decompressed data.
 * Official name: rzipUncompress
 */
u8 *gzip_inflate(u8 *compressedInput, u8 *decompressedOutput) {
    gzip_inflate_input = compressedInput + 5; // The compression header is 5 bytes.
    gzip_inflate_output = decompressedOutput;
    gzip_num_bits = 0;
    gzip_bit_buffer = 0;
    while (gzip_inflate_block() != 0) {} // Keep calling gzip_inflate_block() until it returns 0.
    return decompressedOutput;
}

/* Official name: huft_build */
void gzip_huft_build(u32 *b, u32 n, u32 s, u16 *d, u16 *e, huft **t, s32 *m) {
    u32 a;                   /* counter for codes of length k */
    u32 c[BMAX + 1];         /* bit length count table */
    u32 f;                   /* i repeats in table every f entries */
    s32 g;                   /* maximum code length */
    s32 h;                   /* table level */
    register u32 i;          /* counter, current code */
    register u32 j;          /* counter */
    register s32 k;          /* number of bits in current code */
    s32 l;                   /* bits per table (returned in m) */
    register u32 *p;         /* pointer into c[], b[], or v[] */
    register struct huft *q; /* points to current table */
    struct huft r;           /* table entry for structure assignment */
    struct huft *u[BMAX];    /* table stack */
    u32 v[N_MAX];            /* values in order of bit length */
    register s32 w;          /* bits before this table == (l * h) */
    u32 x[BMAX + 1];         /* bit offsets, then code stack */
    u32 *xp;                 /* pointer into x */
    s32 y;                   /* number of dummy codes added */
    u32 z;                   /* number of entries in current table */

    /* Generate counts for each bit length */
    bzero(c, sizeof(c));
    p = b;
    i = n;
    do {
        c[*p]++; /* assume all entries <= BMAX */
        p++;     /* Can't combine with above line (Solaris bug) */
    } while (--i);
    if (c[0] == n) /* null input--all zero length codes */
    {
        *t = NULL;
        *m = 0;
        return;
    }

    /* Find minimum and maximum length, bound *m by those */
    l = *m;
    for (j = 1; j <= BMAX; j++) {
        if (c[j]) {
            break;
        }
    }
    k = j; /* minimum code length */
    if ((u32) l < j) {
        l = j;
    }
    for (i = BMAX; i; i--) {
        if (c[i]) {
            break;
        }
    }
    g = i; /* maximum code length */
    if ((u32) l > i) {
        l = i;
    }
    *m = l;

    // Something is missing here.
    /* Adjust last length count to fill out codes, if needed */
    y = 1 << j;
    while (j < i) {
        y -= c[j];
        j++;
        y <<= 1;
    }
    y -= c[i];
    c[i] += y;

    /* Generate starting offsets into the value table for each length */
    x[1] = j = 0;
    p = c + 1;
    xp = x + 2;
    while (--i) { /* note that i == g from above */
        *xp++ = (j += *p++);
    }

    /* Make a table of values in order of bit lengths */
    p = b;
    i = 0;
    do {
        if ((j = *p++) != 0) {
            v[x[j]++] = i;
        }
    } while (++i < n);
    // n = x[g];              /* set n to length of v */

    /* Generate the Huffman codes and for each, make the table entries */
    x[0] = i = 0; /* first Huffman code is zero */
    p = v;        /* grab values in bit order */
    h = -1;       /* no tables yet--level -1 */
    w = -l;       /* bits decoded == (l * h) */
    u[0] = NULL;  /* just to keep compilers happy */
    q = NULL;     /* ditto */
    z = 0;        /* ditto */

    // gHuftTable and gHuftTablePos go in here somewhere.

    /* go through the bit lengths (k already is bits in shortest code) */
    for (; k <= g; k++) {
        a = c[k];
        while (a--) {
            /* here i is the Huffman code of length k bits for value *p */
            /* make tables up to required level */
            while (k > w + l) {
                h++;
                w += l; /* previous table always l bits */

                /* compute minimum size table less than or equal to l bits */
                z = (z = g - w) > (u32) l ? (u32) l : z; /* upper limit on table size */
                if ((f = 1 << (j = k - w)) > a + 1)      /* try a k-w bit table */
                {                                        /* too few codes for k-w bit table */
                    f -= a + 1;                          /* deduct codes from patterns left */
                    xp = c + k;
                    while (++j < z) /* try smaller tables up to z bits */
                    {
                        if ((f <<= 1) <= *++xp) {
                            break; /* enough codes to use up j bits */
                        }
                        f -= *xp; /* else deduct codes from patterns */
                    }
                }
                z = 1 << j; /* table entries for j-bit table */

                q = &gHuftTable[gHuftTablePos];
                gHuftTablePos += z + 1;

                *t = q + 1; /* link to list for huft_free() */
                *(t = &(q->v.t)) = NULL;
                u[h] = ++q; /* table starts after link */

                /* connect to last table, if there is one */
                if (h) {
                    x[h] = i;         /* save pattern for backing up */
                    r.b = l;          /* bits to dump before this table */
                    r.e = 16 + j;     /* bits in this table */
                    r.v.t = q;        /* pointer to this table */
                    j = i >> (w - l); /* (get around Turbo C bug) */
                    u[h - 1][j] = r;  /* connect to last table */
                }
            }

            /* set up table entry in r */
            r.b = k - w;
            if (p >= v + n) {
                r.e = 99; /* out of values--invalid code */
            } else if (*p < s) {
                r.e = *p < 256 ? 16 : 15; /* 256 is end-of-block code */
                r.v.n = *p;               /* simple code is just the value */
                p++;                      /* one compiler does not like *p++ */
            } else {
                r.e = ((u8 *) e)[*p - s]; /* non-simple--look up in lists */
                r.v.n = d[*p++ - s];
            }

            /* fill code-like entries with r */
            f = 1 << (k - w);
            for (j = i >> w; j < z; j += f) {
                q[j] = r;
            }

            /* backwards increment the k-bit code i */
            for (j = 1 << (k - 1); i & j; j >>= 1) {
                i ^= j;
            }
            i ^= j;

            /* backup over finished tables */
            while ((i & ((1 << w) - 1)) != x[h]) {
                h--; /* don't need to update q */
                w -= l;
            }
        }
    }
    return;
}

/* Bit-buffer macros for the inflate functions, following the classic gzip
   NEEDBITS/DUMPBITS pattern. Unlike stock gzip there is no input refill
   callback: bytes come straight from the inPtr cursor. b, k and inPtr must
   be locals of the function using these. */
#define NEEDBITS(n)                     \
    while (k < (n)) {                   \
        b |= ((u32) *inPtr++) << k;     \
        k += 8;                         \
    }
#define DUMPBITS(n) \
    {               \
        b >>= (n);  \
        k -= (n);   \
    }

/**
 * Decompress one deflate block and return 0 if it was the last block of the
 * stream, nonzero otherwise (the caller in gzip_inflate() loops until 0).
 * Errors from the per-type handlers are not detectable: like the original
 * hand-written asm, everything below is void and assumes well-formed input.
 */
s32 gzip_inflate_block(void) {
    u32 e; /* last block flag */
    u32 t; /* block type */
    register u32 b; /* bit buffer */
    register u32 k; /* number of bits in bit buffer */
    u8 *inPtr;

    /* reset the huft bump allocator; tables are rebuilt per block */
    gHuftTablePos = 0;

    b = gzip_bit_buffer;
    k = gzip_num_bits;
    inPtr = gzip_inflate_input;

    /* read in last block bit */
    NEEDBITS(1);
    e = b & 1;
    DUMPBITS(1);

    /* read in block type */
    NEEDBITS(2);
    t = b & 3;
    DUMPBITS(2);

    gzip_inflate_input = inPtr;
    gzip_bit_buffer = b;
    gzip_num_bits = k;

    if (t == 2) {
        gzip_inflate_dynamic();
    } else if (t == 1) {
        gzip_inflate_fixed();
    } else {
        /* type 0; an invalid type 3 also lands here, as in the original asm */
        gzip_inflate_stored();
    }

    return 1 - e;
}

/**
 * Decompress an inflated type 2 (dynamic Huffman codes) block.
 */
void gzip_inflate_dynamic(void) {
    huft *tl;           /* literal/length code table */
    huft *td;           /* distance code table */
    s32 bl;             /* lookup bits for tl */
    s32 bd;             /* lookup bits for td */
    u32 nb;             /* number of bit length codes */
    u32 nl;             /* number of literal/length codes */
    u32 nd;             /* number of distance codes */
    u32 j;
    u32 l;              /* last length */
    u32 m;              /* mask for bit lengths table */
    s32 count;          /* number of lengths left to get */
    u32 *llp;           /* cursor into ll */
    huft *t;            /* pointer to table entry */
    u32 ll[288 + 32];   /* literal/length and distance code lengths */
    register u32 b;     /* bit buffer */
    register u32 k;     /* number of bits in bit buffer */
    u8 *inPtr;

    b = gzip_bit_buffer;
    k = gzip_num_bits;
    inPtr = gzip_inflate_input;

    /* read in table lengths */
    NEEDBITS(5);
    nl = 257 + (b & 0x1F); /* number of literal/length codes */
    DUMPBITS(5);
    NEEDBITS(5);
    nd = 1 + (b & 0x1F); /* number of distance codes */
    DUMPBITS(5);
    NEEDBITS(4);
    nb = 4 + (b & 0xF); /* number of bit length codes */
    DUMPBITS(4);

    /* read in bit-length-code lengths */
    for (j = 0; j < nb; j++) {
        NEEDBITS(3);
        ll[gzip_border[j]] = b & 7;
        DUMPBITS(3);
    }
    for (; j < 19; j++) {
        ll[gzip_border[j]] = 0;
    }

    /* build decoding table for trees--single level, 7 bit lookup */
    bl = 7;
    gzip_huft_build(ll, 19, 19, NULL, NULL, &tl, &bl);

    /* read in literal and distance code lengths */
    m = gzip_mask_bits[bl];
    count = nl + nd;
    llp = ll;
    l = 0;
    while (count != 0) {
        NEEDBITS((u32) bl);
        t = tl + (b & m);
        j = t->v.n;
        DUMPBITS(t->b);
        if (j < 16) {        /* length of code in bits (0..15) */
            *llp++ = l = j;  /* save last length in l */
            count--;
        } else if (j == 16) { /* repeat last length 3 to 6 times */
            NEEDBITS(2);
            j = 3 + (b & 3);
            DUMPBITS(2);
            count -= j;
            while (j--) {
                *llp++ = l;
            }
        } else if (j == 17) { /* 3 to 10 zero length codes */
            NEEDBITS(3);
            j = 3 + (b & 7);
            DUMPBITS(3);
            count -= j;
            while (j--) {
                *llp++ = 0;
            }
            l = 0;
        } else { /* j == 18: 11 to 138 zero length codes */
            NEEDBITS(7);
            j = 11 + (b & 0x7F);
            DUMPBITS(7);
            count -= j;
            while (j--) {
                *llp++ = 0;
            }
            l = 0;
        }
    }

    /* restore the global bit buffer */
    gzip_inflate_input = inPtr;
    gzip_bit_buffer = b;
    gzip_num_bits = k;

    /* build the decoding tables for literal/length and distance codes */
    bl = 9; /* lbits */
    gzip_huft_build(ll, nl, 257, gzip_cplens, (u16 *) gzip_cplext, &tl, &bl);
    bd = 6; /* dbits */
    gzip_huft_build(ll + nl, nd, 0, gzip_cpdist, (u16 *) gzip_cpdext, &td, &bd);

    /* decompress until an end-of-block code */
    gzip_inflate_codes(tl, td, bl, bd);
}

/**
 * Decompress an inflated type 1 (fixed Huffman codes) block.
 */
void gzip_inflate_fixed(void) {
    huft *tl; /* literal/length code table */
    huft *td; /* distance code table */
    s32 bl;   /* lookup bits for tl */
    s32 bd;   /* lookup bits for td */
    s32 i;
    u32 l[288]; /* length list for huft_build */

    /* set up literal table */
    for (i = 0; i < 144; i++) {
        l[i] = 8;
    }
    for (; i < 256; i++) {
        l[i] = 9;
    }
    for (; i < 280; i++) {
        l[i] = 7;
    }
    for (; i < 288; i++) { /* make a complete, but wrong code set */
        l[i] = 8;
    }
    /* Note: the original asm stored its bl/bd seed values (7 and 5) to dead
       stack slots and passed huft_build pointers to uninitialized ones; it
       only worked because huft_build clamps *m into the [min,max] code
       length range. Initializing them properly is behavior-neutral. */
    bl = 7;
    gzip_huft_build(l, 288, 257, gzip_cplens, (u16 *) gzip_cplext, &tl, &bl);

    /* set up distance table */
    for (i = 0; i < 30; i++) { /* make an incomplete code set */
        l[i] = 5;
    }
    bd = 5;
    gzip_huft_build(l, 30, 0, gzip_cpdist, (u16 *) gzip_cpdext, &td, &bd);

    /* decompress until an end-of-block code */
    gzip_inflate_codes(tl, td, bl, bd);
}

/**
 * "Decompress" an inflated type 0 (stored) block.
 */
void gzip_inflate_stored(void) {
    u32 n;          /* number of bytes in block */
    register u32 b; /* bit buffer */
    register u32 k; /* number of bits in bit buffer */
    u8 *inPtr;
    u8 *outPtr;

    b = gzip_bit_buffer;
    k = gzip_num_bits;
    inPtr = gzip_inflate_input;
    outPtr = gzip_inflate_output;

    /* go to byte boundary */
    n = k & 7;
    DUMPBITS(n);

    /* get the length and its complement (the complement is read but,
       unlike stock gzip, never verified) */
    NEEDBITS(16);
    n = b & 0xFFFF;
    DUMPBITS(16);
    NEEDBITS(16);
    DUMPBITS(16);

    /* After the byte-align, the two 16-bit reads leave the bit buffer
       holding exactly 0 bits, so the stored bytes can be copied straight
       from the input cursor. */
    while (n != 0) {
        *outPtr++ = *inPtr++;
        n--;
    }

    gzip_inflate_input = inPtr;
    gzip_inflate_output = outPtr;
    gzip_bit_buffer = b;
    gzip_num_bits = k;
}

/**
 * Inflate (decompress) the codes in a deflated (compressed) block.
 * tl, td: literal/length and distance decoder tables.
 * bl, bd: number of bits decoded by tl[] and td[].
 * Unlike stock gzip this writes through the output cursor directly (no 32K
 * sliding window) and has no invalid-code (e == 99) check.
 */
void gzip_inflate_codes(huft *tl, huft *td, s32 bl, s32 bd) {
    u32 e;          /* table entry flag/number of extra bits */
    u32 n;          /* length for copy */
    huft *t;        /* pointer to table entry */
    u32 ml, md;     /* masks for bl and bd bits */
    u8 *src;        /* source for back-reference copy */
    register u32 b; /* bit buffer */
    register u32 k; /* number of bits in bit buffer */
    u8 *inPtr;
    u8 *outPtr;

    b = gzip_bit_buffer;
    k = gzip_num_bits;
    inPtr = gzip_inflate_input;
    outPtr = gzip_inflate_output;

    ml = gzip_mask_bits[bl]; /* precompute masks for speed */
    md = gzip_mask_bits[bd];
    for (;;) { /* do until end of block */
        NEEDBITS((u32) bl);
        t = tl + (b & ml);
        e = t->e;
        while (e > 16) { /* walk subtables */
            DUMPBITS(t->b);
            e -= 16;
            NEEDBITS(e);
            t = t->v.t + (b & gzip_mask_bits[e]);
            e = t->e;
        }
        DUMPBITS(t->b);
        if (e == 16) { /* then it's a literal */
            *outPtr++ = t->v.n;
            continue;
        }

        /* exit if end of block */
        if (e == 15) {
            break;
        }

        /* get length of block to copy */
        NEEDBITS(e);
        n = t->v.n + (b & gzip_mask_bits[e]);
        DUMPBITS(e);

        /* decode distance of block to copy */
        NEEDBITS((u32) bd);
        t = td + (b & md);
        e = t->e;
        while (e > 16) {
            DUMPBITS(t->b);
            e -= 16;
            NEEDBITS(e);
            t = t->v.t + (b & gzip_mask_bits[e]);
            e = t->e;
        }
        DUMPBITS(t->b);
        NEEDBITS(e);
        src = outPtr - t->v.n - (b & gzip_mask_bits[e]);
        DUMPBITS(e);

        /* do the copy; must be a forward byte-by-byte copy since the
           source can overlap the bytes being written (LZ77 runs) */
        do {
            *outPtr++ = *src++;
        } while (--n != 0);
    }

    gzip_inflate_input = inPtr;
    gzip_inflate_output = outPtr;
    gzip_bit_buffer = b;
    gzip_num_bits = k;
}
