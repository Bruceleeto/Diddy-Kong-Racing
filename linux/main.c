#include <stdio.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>

#include "gfx.h"

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread, same as
// the OoT DC port calling Main()/Graph_ThreadEntry directly — the N64 boot
// chain (mainproc -> thread1 -> thread3) is skipped entirely, so the libultra
// thread machinery is never needed.
void thread3_main(void *unused);

// libultra code asks about "the current thread" (osGetThreadPri(NULL) etc.).
// There is no thread system on PC, so the host thread poses as one: thread 3
// at its real priority.
extern OSThread *__osRunningThread;
static OSThread sHostThread;

// ---------------------------------------------------------------------------
// Frame pacing — stands in for the VI retrace interrupt. The return value is
// the game's logic update rate: the number of retrace periods this frame covers,
// which obj_update() uses directly as its physics timestep.
//
// It must be STABLE, not merely accurate. Retail fb_update() (src/video.c)
// commits to a rate (gVideoDeltaTime, initialised to LOGIC_30FPS = 2) and
// *blocks* to pad a fast frame out to it, only lowering the rate after 20
// consecutive frames disagree. Returning the raw measured period count instead
// lets the timestep oscillate 1,2,1,1,2 with wall-clock noise, and integrating
// physics with a jittering timestep makes everything visibly shake in place.
//
// So: pace to the same 30Hz cadence vanilla runs at, and only report more
// periods if we genuinely ran slow.
// ---------------------------------------------------------------------------
#include <time.h>

#define PC_RETRACE_NSEC (1000000000ll / 60)
#define PC_LOGIC_UPDATE_RATE 2 // LOGIC_30FPS — what gVideoDeltaTime commits to
#define PC_MAX_UPDATE_RATE 6   // don't let a debugger pause become a huge skip

s32 pc_retrace_wait(void) {
    static long long sLastNs = 0;
    long long targetNs;
    struct timespec ts;
    long long nowNs;
    s32 periods;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    nowNs = (long long) ts.tv_sec * 1000000000ll + ts.tv_nsec;

    if (sLastNs == 0) {
        sLastNs = nowNs;
        return PC_LOGIC_UPDATE_RATE;
    }

    // Sleep until this frame has covered its full committed cadence. This is
    // the stand-in for retail's blocking osRecvMesg on the vblank queue.
    targetNs = sLastNs + (PC_LOGIC_UPDATE_RATE * PC_RETRACE_NSEC);
    if (nowNs < targetNs) {
        struct timespec req;
        req.tv_sec = (targetNs - nowNs) / 1000000000ll;
        req.tv_nsec = (targetNs - nowNs) % 1000000000ll;
        nanosleep(&req, NULL);
        nowNs = targetNs;
    }

    periods = (s32) ((nowNs - sLastNs) / PC_RETRACE_NSEC);
    if (periods < PC_LOGIC_UPDATE_RATE) {
        periods = PC_LOGIC_UPDATE_RATE;
    }
    if (periods > PC_MAX_UPDATE_RATE) {
        periods = PC_MAX_UPDATE_RATE;
    }

    // Advance by whole periods so rounding doesn't accumulate into drift.
    sLastNs += (long long) periods * PC_RETRACE_NSEC;
    return periods;
}

// ---------------------------------------------------------------------------
// F3DDKR HLE renderer — untextured, vertex-shaded.
//
// gfxtask_run_xbus (src/rcp_dkr.c) hands us the display list the RSP would have
// run. This interprets the geometry half of the microcode the way the RSP does
// — matrix slots, vertex loads, billboarding, G_TRIN polygons — transforms to
// clip space, perspective divides, maps through the viewport, and hands the
// triangles to the host GL layer (linux/gfx.c), shaded from their vertex
// colours. Everything RDP-side (textures, combiners, blenders, rectangles) is
// ignored for now.
// ---------------------------------------------------------------------------

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define WINDOW_SCALE 3

#define GFX_MAX_VERTS 64    // the RSP's internal vertex array
#define GFX_MAX_DEPTH 16    // display-list recursion guard
#define GFX_MAX_TRI_VERTS 65536

// A vertex in clip space — post-matrix, pre-divide. Kept unprojected because
// near-plane clipping has to interpolate here, before the divide by w (a vertex
// behind the camera has w <= 0, and dividing by it is exactly the garbage the
// clip exists to prevent).
typedef struct {
    f32 clip[4];
    f32 u, v; // texel coords, filled in per-triangle from the Triangle's UVs
    u8 r, g, b, a;
} GfxVertex;

// ---------------------------------------------------------------------------
// RDP texture state. DKR uses the stock texture commands (gDPLoadTextureBlock
// and friends), which arrive as G_SETTIMG (where the image is) + G_SETTILE (how
// to read it) + G_SETTILESIZE (how big it is) + G_LOADTLUT (palette, for CI).
// TMEM is not emulated: at draw time we decode straight from the image address
// the display list last pointed at, which is what every N64 HLE renderer does
// and works because the game's textures are laid out linearly in RAM.
// ---------------------------------------------------------------------------

// Clip anything closer than this. The N64 clips against w, and w is the
// camera-space depth, so this is the near plane in world units.
#define GFX_NEAR_W 1.0f

// Which screen-space winding is a front face. Screen y runs downwards here, so
// this is the opposite sign from the y-up convention. If the world renders
// inside-out — outer surfaces gone, inner ones visible — flip this.
#define GFX_FRONT_FACE_SIGN (-1.0f)

static u32 sGfxFrameCount = 0;

static f32 sMatrices[3][4][4]; // G_MTX_DKR_INDEX_0..2
static s32 sCurMatrix = 0;
static s32 sBillboard = FALSE;

static GfxVertex sVerts[GFX_MAX_VERTS];
static s32 sVertexBase = 0; // where G_VTX_APPEND vertices land

static f32 sVpScaleX = N64_SCREEN_W / 2.0f;
static f32 sVpScaleY = N64_SCREEN_H / 2.0f;
static f32 sVpTransX = N64_SCREEN_W / 2.0f;
static f32 sVpTransY = N64_SCREEN_H / 2.0f;

static GfxTriVert sTriVerts[GFX_MAX_TRI_VERTS];
static s32 sTriVertCount = 0;

static u32 sTimgAddr = 0;  // G_SETTIMG — wherever the DL last pointed
static u32 sTexAddr = 0;   // the image a G_LOADBLOCK/G_LOADTILE actually loaded
static s32 sTexSwapped = 0; // that load had dxt == 0 — see decode_texture()
static u8 sTileFmt = 0;    // G_SETTILE — G_IM_FMT_*
static u8 sTileSiz = 0;    //             G_IM_SIZ_*
static u8 sTilePalette = 0;
static u8 sTileCmS = 0; // the raw 2-bit clamp/mirror fields, not booleans
static u8 sTileCmT = 0;
static u16 sTileWidth = 0; // G_SETTILESIZE
static u16 sTileHeight = 0;
static u16 sTileUls = 0;   //               the tile's origin within the image
static u16 sTileUlt = 0;
static u32 sTlutAddr = 0;  // G_LOADTLUT — palette for the CI formats

// RDP colour state. Only the 2D path reads these: the triangles get their colour
// from the vertices, but a rectangle has no vertex colours, so its colour comes
// entirely from the combiner and these registers.
static u32 sOtherModeH = 0;      // G_SETOTHERMODE_H / G_RDPSETOTHERMODE
static u32 sOtherModeL = 0;      // G_SETOTHERMODE_L / G_RDPSETOTHERMODE
static u32 sCombineW0 = 0;       // G_SETCOMBINE — the two mux words
static u32 sCombineW1 = 0;
static u8 sPrimColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sEnvColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sFillColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static u8 sBlendColor[4] = { 0x00, 0x00, 0x00, 0xFF }; // G_SETBLENDCOLOR

#define GFX_MAX_TEXTURES 1024
#define GFX_MAX_TEX_TEXELS (512 * 512)

typedef struct {
    u32 timg;
    u32 tlut;
    u8 fmt, siz, cmS, cmT;
    u8 swapped;
    u16 width, height;
    u32 handle;
    u32 lastUsed; // frame number, for eviction when the cache is full
} GfxTexture;

static GfxTexture sTexCache[GFX_MAX_TEXTURES];
static s32 sTexCacheCount = 0;
static u32 sTexDecodeBuf[GFX_MAX_TEX_TEXELS];
static u8 sTexSwizzleBuf[GFX_MAX_TEX_TEXELS * 4]; // worst case: 32bpp

/**
 * Drop every cached texture decoded out of the memory being freed.
 *
 * The cache is keyed on the RAM address a texture was decoded from, and the game
 * recycles those addresses: the textures of a level it has unloaded are handed
 * straight back out to the next one. Without this, a new texture landing on an
 * old address with the same format and size hits the stale entry and draws the
 * previous level's pixels — and the cache, never evicting, fills up and starts
 * refusing to decode anything at all.
 *
 * This takes the freed slot's whole *range*, not just its base address, and that
 * matters: an earlier version compared against the base and therefore matched
 * nothing, ever. A texture's pixels begin at `tex + 1` — past its TextureHeader —
 * and its palette sits at another offset again (`tex_palette_id`), so no cache
 * entry is ever keyed on an allocation's base. The invalidation silently did
 * nothing, which is exactly the two symptoms you would predict: stale textures
 * after a level change, then everything untextured once the cache hit its cap.
 *
 * Called from mempool_free_addr() (src/memory.c), the choke point every free in
 * the game passes through, so a texture's GL object dies exactly when the memory
 * behind it does. The palette is checked too: a CI texture decoded against a
 * freed TLUT is just as stale.
 */
void pc_gfx_invalidate_range(const void *addr, s32 size) {
    u32 lo = (u32) addr;
    u32 hi = lo + (u32) size;
    s32 i;

    for (i = 0; i < sTexCacheCount;) {
        GfxTexture *t = &sTexCache[i];

        if ((t->timg >= lo && t->timg < hi) || (t->tlut >= lo && t->tlut < hi)) {
            gfx_delete_texture(t->handle);
            sTexCache[i] = sTexCache[--sTexCacheCount];
        } else {
            i++;
        }
    }
}

/**
 * Unpack an N64 Mtx (s15.16 fixed point: the integer halves live in the first 8
 * words, the fractional halves in the last 8) into floats — the inverse of
 * mtxf_to_mtx() in src/hasm/math_util.c. Deliberately kept in the same format
 * the RSP consumes, so the PC build sees the same rounding and the same +-32768
 * saturation the console does and the N64 ROM stays a usable oracle.
 */
static void mtx_to_float(const Mtx *m, f32 out[4][4]) {
    const s32 *ints = (const s32 *) &m->m[0][0];
    const s32 *fracs = ints + 8;
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 idx = (i * 2) + (j >> 1);
            s32 fixed;

            if ((j & 1) == 0) {
                fixed = (ints[idx] & 0xFFFF0000) | ((fracs[idx] >> 16) & 0xFFFF);
            } else {
                fixed = (ints[idx] << 16) | (fracs[idx] & 0xFFFF);
            }
            out[i][j] = (f32) fixed / 65536.0f;
        }
    }
}

/**
 * Transform a vertex the way the RSP does: as a row vector times the selected
 * matrix (the same convention as mtxf_transform_point), then — when
 * billboarding — add the anchor's clip coordinates, then perspective divide and
 * map through the viewport.
 */
static void load_vertex(GfxVertex *dst, const Vertex *v, const f32 *anchor) {
    const f32 (*m)[4] = sMatrices[sCurMatrix];
    f32 x = v->x, y = v->y, z = v->z;
    s32 i;

    for (i = 0; i < 4; i++) {
        dst->clip[i] = (m[0][i] * x) + (m[1][i] * y) + (m[2][i] * z) + m[3][i];
        if (anchor != NULL) {
            dst->clip[i] += anchor[i];
        }
    }

    dst->r = v->r;
    dst->g = v->g;
    dst->b = v->b;
    // Shade alpha. The combiner uses it as a blend weight (SHADE_ALPHA), and the
    // RENDER_VTX_ALPHA materials use it as real translucency — which is why it is
    // mutually exclusive with fog in material_set(): the RSP keeps vertex alpha in
    // the fog slot.
    dst->a = v->a;
}

static void handle_vertex(u32 w0, u32 w1) {
    const Vertex *src = (const Vertex *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = ((params >> 3) & 0x1F) + 1;
    s32 append = params & G_VTX_APPEND;
    s32 dstIdx;
    s32 i;

    if (src == NULL) {
        return;
    }

    if (append) {
        dstIdx = sVertexBase;
    } else {
        // Non-appended vertices always go to the start of the array, and their
        // count becomes the base that appended vertices land after.
        dstIdx = 0;
        sVertexBase = count;
    }

    for (i = 0; i < count && dstIdx + i < GFX_MAX_VERTS; i++) {
        // Billboarded vertices are sprite-space offsets from vertex 0, the
        // anchor pushed just before the billboard matrix.
        load_vertex(&sVerts[dstIdx + i], &src[i], sBillboard ? sVerts[0].clip : NULL);
    }

}

/** Expand an N64 RGBA5551 texel (big-endian in the asset) to RGBA8888. */
static u32 rgba16_to_rgba32(u16 texel) {
    u32 r = (texel >> 11) & 0x1F;
    u32 g = (texel >> 6) & 0x1F;
    u32 b = (texel >> 1) & 0x1F;
    u32 a = (texel & 1) ? 0xFF : 0x00;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return r | (g << 8) | (b << 16) | (a << 24);
}

static u32 ia_to_rgba32(u32 intensity, u32 alpha) {
    return intensity | (intensity << 8) | (intensity << 16) | (alpha << 24);
}

/**
 * Undo the N64's odd-row word swizzle.
 *
 * TMEM stores odd rows of a texture with the two 32-bit halves of each 64-bit
 * word exchanged. A normal gDPLoadTextureBlock passes a nonzero dxt, which makes
 * the RDP apply that swizzle as it streams the block in, and the texture fetch
 * unit undoes it on the way out — so the bytes in RAM are plain linear and we can
 * read them as-is.
 *
 * DKR's RENDER_LINE_SWAP textures ("Texture has swapped lines, for speed") do not
 * work that way. They go through gDPLoadTextureBlockS, whose only difference is
 * that it passes dxt = 0 (include/PR/gbi.h:2639) — so the RDP does no swizzle on
 * load, and the asset is instead stored *pre-swizzled* in ROM, letting the fetch
 * unit's unswizzle produce the correct image for free. Reading those bytes
 * linearly, as we did, leaves every odd row with its 4-byte groups exchanged in
 * pairs: the sprite stays entirely recognisable but its odd scanlines are
 * displaced in short runs, which is the serrated comb along the edges of the HUD
 * text.
 *
 * So: when the load had dxt == 0, walk the rows back through the same swap before
 * decoding. `b ^ 4` is the swizzle; it is its own inverse. Rows narrower than 8
 * bytes have nothing to exchange, and the bounds check covers a trailing partial
 * group.
 */
static void unswizzle_rows(const u8 *src, u8 *dst, u8 siz, s32 rowBytes, s32 height) {
    // The swizzle exchanges the 32-bit halves of each 64-bit TMEM word, so it is
    // a ^4 on the byte address *within TMEM*. For 4/8/16-bit texels, TMEM holds
    // the image exactly as RAM does and the ^4 carries straight over.
    //
    // 32-bit texels do not: the RDP splits them across two TMEM banks, red/green
    // in the low one and blue/alpha in the high (which is why the GBI computes
    // their line with G_IM_SIZ_32b_LINE_BYTES = 2, not 4). Each bank therefore
    // holds 2 bytes per texel, so a ^4 in bank-local address space exchanges
    // *pairs* of texels — texel ^ 2 — which in the linear 4-byte-per-texel RAM
    // image we decode from is a ^8. Using ^4 here instead scrambles the texels
    // singly rather than in pairs: still recognisable, subtly wrong. That was the
    // static banana.
    s32 unit = (siz == G_IM_SIZ_32b) ? 8 : 4;
    s32 total = rowBytes * height;
    s32 y, b;

    for (y = 0; y < height; y++) {
        for (b = 0; b < rowBytes; b++) {
            // The ^ is against the offset into the whole image, not into the row:
            // a block load fills TMEM contiguously, and the two only coincide
            // when a row is a whole number of 64-bit words.
            s32 at = (y * rowBytes) + b;
            s32 from = (y & 1) ? (at ^ unit) : at;

            dst[at] = src[(from < total) ? from : at];
        }
    }
}

/** How many bytes one row of a `width`-texel image of this size occupies. */
static s32 row_bytes(u8 siz, s32 width) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return (width + 1) / 2;
        case G_IM_SIZ_8b:
            return width;
        case G_IM_SIZ_16b:
            return width * 2;
        default:
            return width * 4;
    }
}

/**
 * Decode one of the N64 texture formats into RGBA8888. Everything is read a
 * byte at a time, so the assets staying big-endian doesn't matter here.
 */
static void decode_texture(const u8 *src, u8 fmt, u8 siz, s32 width, s32 height, const u8 *tlut, u32 *out) {
    s32 count = width * height;
    s32 i;

    switch ((fmt << 2) | siz) {
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = rgba16_to_rgba32((src[i * 2] << 8) | src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_32b:
            for (i = 0; i < count; i++) {
                out[i] = src[i * 4] | (src[(i * 4) + 1] << 8) | (src[(i * 4) + 2] << 16) | (src[(i * 4) + 3] << 24);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 idx = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 idx = src[i];
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i * 2], src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 hi = src[i] >> 4;
                u32 lo = src[i] & 0xF;
                out[i] = ia_to_rgba32((hi << 4) | hi, (lo << 4) | lo);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                u32 intensity = (texel >> 1) & 7;
                intensity = (intensity << 5) | (intensity << 2) | (intensity >> 1);
                out[i] = ia_to_rgba32(intensity, (texel & 1) ? 0xFF : 0x00);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i], src[i]);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                texel = (texel << 4) | texel;
                out[i] = ia_to_rgba32(texel, texel);
            }
            break;
        default:
            for (i = 0; i < count; i++) {
                out[i] = 0xFFFF00FF; // magenta: an unhandled format, so it's obvious
            }
            break;
    }
}

/**
 * The texture the current RDP state describes, decoded and uploaded once and
 * then kept. Returns 0 (untextured) if there is nothing sane to draw with.
 */
static u32 texture_current(void) {
    const u8 *tlut = (const u8 *) sTlutAddr;
    s32 i;

    if (sTexAddr == 0 || sTileWidth == 0 || sTileHeight == 0) {
        return 0;
    }
    if (sTileWidth * sTileHeight > GFX_MAX_TEX_TEXELS) {
        return 0;
    }
    if (sTileFmt == G_IM_FMT_CI) {
        if (sTlutAddr == 0) {
            return 0;
        }
        // CI4 palettes are 16 entries each, selected by the tile's palette field.
        if (sTileSiz == G_IM_SIZ_4b) {
            tlut += sTilePalette * 16 * 2;
        }
    }

    for (i = 0; i < sTexCacheCount; i++) {
        GfxTexture *t = &sTexCache[i];
        if (t->timg == sTexAddr && t->tlut == (u32) tlut && t->fmt == sTileFmt && t->siz == sTileSiz &&
            t->width == sTileWidth && t->height == sTileHeight && t->cmS == sTileCmS && t->cmT == sTileCmT &&
            t->swapped == (u8) sTexSwapped) {
            t->lastUsed = sGfxFrameCount;
            return t->handle;
        }
    }

    // A full cache used to mean "draw untextured, forever". Evict the entry that
    // has gone unused the longest instead — a texture the game still wants will
    // simply be decoded again next time it asks for it. With invalidation working
    // this should not trigger, but degrading into a slow frame beats degrading
    // into a permanently untextured world.
    if (sTexCacheCount >= GFX_MAX_TEXTURES) {
        s32 oldest = 0;

        for (i = 1; i < sTexCacheCount; i++) {
            if (sTexCache[i].lastUsed < sTexCache[oldest].lastUsed) {
                oldest = i;
            }
        }
        gfx_delete_texture(sTexCache[oldest].handle);
        sTexCache[oldest] = sTexCache[--sTexCacheCount];
    }

    {
        const u8 *texels = (const u8 *) sTexAddr;
        s32 rowBytes = row_bytes(sTileSiz, sTileWidth);

        // A dxt of 0 means the asset is stored pre-swizzled; put it back before
        // decoding. Guarded on the scratch buffer, which a sane texture never
        // exceeds — decoding the raw bytes is better than reading past it.
        if (sTexSwapped && (rowBytes * sTileHeight) <= (s32) sizeof(sTexSwizzleBuf)) {
            unswizzle_rows(texels, sTexSwizzleBuf, sTileSiz, rowBytes, sTileHeight);
            texels = sTexSwizzleBuf;
        }
        decode_texture(texels, sTileFmt, sTileSiz, sTileWidth, sTileHeight, tlut, sTexDecodeBuf);
    }

    {
        GfxTexture *t = &sTexCache[sTexCacheCount++];
        t->timg = sTexAddr;
        t->tlut = (u32) tlut;
        t->fmt = sTileFmt;
        t->siz = sTileSiz;
        t->swapped = (u8) sTexSwapped;
        t->width = sTileWidth;
        t->height = sTileHeight;
        t->cmS = sTileCmS;
        t->cmT = sTileCmT;
        t->lastUsed = sGfxFrameCount;
        t->handle = gfx_create_texture(sTexDecodeBuf, sTileWidth, sTileHeight, sTileCmS, sTileCmT);
        return t->handle;
    }
}

/**
 * Perspective divide plus the viewport map — the last thing the RSP does before
 * handing a vertex to the RDP. Only ever called on clipped vertices, so w is
 * guaranteed positive here.
 */
static void combiner_eval(const f32 shade[4], u8 out[4]);

static void project(const GfxVertex *v, GfxTriVert *out) {
    f32 invW = 1.0f / v->clip[3];

    out->x = (v->clip[0] * invW * sVpScaleX) + sVpTransX;
    out->y = sVpTransY - (v->clip[1] * invW * sVpScaleY);
    // Negated so nearer geometry gets the smaller depth under GL_LESS.
    out->z = -(v->clip[2] * invW);
    // Kept so the host layer can restore the homogeneous position and get
    // perspective-correct texturing out of the fixed-function pipeline. The near
    // clip guarantees this is >= GFX_NEAR_W, so it is safe to multiply back by.
    out->w = v->clip[3];
    out->u = v->u;
    out->v = v->v;

    // Run the combiner on this vertex's shade. The texture unit modulates the
    // texel in afterwards, so what comes out here is everything the RDP would
    // have computed *around* the texel: the environment blend, the prim colour,
    // and the prim/vertex alpha that drives every fade in the game.
    {
        f32 shade[4];
        u8 col[4];

        shade[0] = v->r / 255.0f;
        shade[1] = v->g / 255.0f;
        shade[2] = v->b / 255.0f;
        shade[3] = v->a / 255.0f;
        combiner_eval(shade, col);

        out->r = col[0];
        out->g = col[1];
        out->b = col[2];
        out->a = col[3];
    }
}

/**
 * Project a triangle and, unless it is marked double-sided, drop it if it is
 * facing away. Winding only exists after the perspective divide, which is why
 * this can't happen back in clip space.
 */
static void push_tri(const GfxVertex *a, const GfxVertex *b, const GfxVertex *c, s32 cull) {
    GfxTriVert v[3];
    f32 area;

    if (sTriVertCount + 3 > GFX_MAX_TRI_VERTS) {
        return;
    }

    project(a, &v[0]);
    project(b, &v[1]);
    project(c, &v[2]);

    if (cull) {
        area = ((v[1].x - v[0].x) * (v[2].y - v[0].y)) - ((v[2].x - v[0].x) * (v[1].y - v[0].y));
        if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
            return;
        }
    }

    sTriVerts[sTriVertCount++] = v[0];
    sTriVerts[sTriVertCount++] = v[1];
    sTriVerts[sTriVertCount++] = v[2];
}

/**
 * Split the edge a->b where it crosses the near plane, at the point where
 * w == GFX_NEAR_W. Colour interpolates linearly in clip space along with the
 * position, which is what the RSP's own clipper does.
 */
static void clip_edge(const GfxVertex *a, const GfxVertex *b, GfxVertex *out) {
    f32 t = (GFX_NEAR_W - a->clip[3]) / (b->clip[3] - a->clip[3]);
    s32 i;

    for (i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + ((b->clip[i] - a->clip[i]) * t);
    }
    out->u = a->u + ((b->u - a->u) * t);
    out->v = a->v + ((b->v - a->v) * t);
    out->r = (u8) (a->r + ((f32) (b->r - a->r) * t));
    out->g = (u8) (a->g + ((f32) (b->g - a->g) * t));
    out->b = (u8) (a->b + ((f32) (b->b - a->b) * t));
    out->a = (u8) (a->a + ((f32) (b->a - a->a) * t));
}

/**
 * Clip a triangle against the near plane (Sutherland-Hodgman on the single
 * w >= GFX_NEAR_W plane), then fan-triangulate whatever polygon survives and
 * emit it. One clipped triangle yields 0, 1 or 2 output triangles.
 */
static void emit_triangle(const GfxVertex *v0, const GfxVertex *v1, const GfxVertex *v2, s32 cull) {
    const GfxVertex *in[3];
    GfxVertex poly[4];
    s32 numOut = 0;
    s32 i;

    in[0] = v0;
    in[1] = v1;
    in[2] = v2;

    for (i = 0; i < 3; i++) {
        const GfxVertex *cur = in[i];
        const GfxVertex *next = in[(i + 1) % 3];
        s32 curIn = cur->clip[3] >= GFX_NEAR_W;
        s32 nextIn = next->clip[3] >= GFX_NEAR_W;

        if (curIn) {
            poly[numOut++] = *cur;
        }
        if (curIn != nextIn) {
            clip_edge(cur, next, &poly[numOut++]);
        }
    }

    if (numOut < 3) {
        return; // entirely behind the near plane
    }

    for (i = 2; i < numOut; i++) {
        push_tri(&poly[0], &poly[i - 1], &poly[i], cull);
    }
}

/**
 * Push the RDP's render mode — the low half of othermode — at the host.
 *
 * DKR never sets this inline: material_set() (src/textures_sprites.c) DMAs in a
 * two-command display list per material, a gsDPSetCombineLERP and a
 * gsDPSetOtherMode, and the render mode is the low word of the latter. So this
 * runs once per G_TRIN, which is once per material batch.
 *
 * The render-mode bits sit at their final positions within othermode-low (the
 * field starts at G_MDSFT_RENDERMODE, which is where AA_EN's 0x8 comes from), so
 * they can be tested against sOtherModeL directly.
 *
 * The blender proper (the GBL_c1/c2 muxes) is still not modelled — everything
 * goes through one fixed src-alpha blend. What is modelled is the part that was
 * doing visible damage: depth compare, depth write, decal offset and alpha
 * compare.
 */
/**
 * G_TF_POINT vs. G_TF_BILERP, from othermode-H. The game point-samples its font
 * and most of its UI art and bilerps the world; filtering everything, as we used
 * to, blurs the text and — because the filter taps reach outside the glyph —
 * drags colour in from whatever the wrap mode puts there.
 *
 * Applies to whatever texture is currently bound, so call this after binding.
 */
static void apply_texture_filter(void) {
    u32 filt = sOtherModeH & (3 << G_MDSFT_TEXTFILT);

    gfx_set_texture_filter(filt == G_TF_POINT);
}

static void apply_render_mode(void) {
    u32 alphaCompare = sOtherModeL & (3 << G_MDSFT_ALPHACOMPARE);
    f32 ref = 0.0f;

    gfx_set_depth_test((sOtherModeL & Z_CMP) != 0);
    gfx_set_depth_write((sOtherModeL & Z_UPD) != 0);
    gfx_set_depth_offset((sOtherModeL & ZMODE_DEC) == ZMODE_DEC);

    // CVG_X_ALPHA is how the cutout materials (G_RM_*_TEX_EDGE) get their hard
    // edge: the RDP multiplies coverage by alpha, which on a non-antialiased
    // host is a straight 50% cutout. Otherwise a threshold compare comes from
    // the blend colour's alpha, and G_AC_DITHER — which DKR only uses where a
    // dithered edge is cosmetic — degrades to the same "drop the invisible
    // texels" default as G_AC_NONE.
    if (sOtherModeL & CVG_X_ALPHA) {
        ref = 0.5f;
    } else if (alphaCompare == G_AC_THRESHOLD) {
        ref = sBlendColor[3] / 255.0f;
    }
    gfx_set_alpha_test(ref);
}

/**
 * G_TRIN — DKR's polygon command. w1 points at an array of Triangles (vertex
 * indices plus UVs); shade each one from its vertex colours.
 */
static void handle_polygon(u32 w0, u32 w1) {
    const Triangle *tris = (const Triangle *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = (params >> 4) + 1;
    s32 texEnabled = params & 1;
    u32 texture = 0;
    f32 invTexW = 0.0f, invTexH = 0.0f;
    s32 i;

    if (tris == NULL) {
        return;
    }

    if (texEnabled) {
        texture = texture_current();
    }
    if (texture != 0) {
        // UVs are S10.5 texel coordinates (32 = one texel), so normalising is a
        // divide by 32 and then by the texture's size.
        invTexW = 1.0f / (32.0f * (f32) sTileWidth);
        invTexH = 1.0f / (32.0f * (f32) sTileHeight);
    }

    // Each G_TRIN is one material batch, so it becomes one draw call.
    sTriVertCount = 0;

    for (i = 0; i < count; i++) {
        GfxVertex v[3];

        v[0] = sVerts[tris[i].vi0 % GFX_MAX_VERTS];
        v[1] = sVerts[tris[i].vi1 % GFX_MAX_VERTS];
        v[2] = sVerts[tris[i].vi2 % GFX_MAX_VERTS];

        v[0].u = tris[i].uv0.u * invTexW;
        v[0].v = tris[i].uv0.v * invTexH;
        v[1].u = tris[i].uv1.u * invTexW;
        v[1].v = tris[i].uv1.v * invTexH;
        v[2].u = tris[i].uv2.u * invTexW;
        v[2].v = tris[i].uv2.v * invTexH;

        // DKR marks culling per triangle: BACKFACE_DRAW means double-sided.
        emit_triangle(&v[0], &v[1], &v[2], !(tris[i].flags & BACKFACE_DRAW));
    }

    apply_render_mode();
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_draw_tris(sTriVerts, sTriVertCount);
}

// ---------------------------------------------------------------------------
// The colour combiner.
//
// Evaluated on the CPU, per vertex, with TEXEL0/TEXEL1 taken as white — the GL
// texture unit then modulates the real texel in afterwards. That is an
// approximation (the RDP would multiply the texel only into the terms that
// actually name it, where we multiply it into the whole result), but it is exact
// for every combiner that reduces to "texel times a constant", and close for the
// rest. It also needs no shader, no GL_COMBINE and no multitexture, which is the
// same trade the OoT Dreamcast port makes.
//
// The important part is that SHADE is a *real input* now. DKR's directionally-lit
// materials (dRenderSettingsDirectionalLighting, used by objects.c whenever
// directional_lighting_on() is in effect — the intro cutscene, for one) are
//
//     cycle 1: G_CC_BLEND_SHADEALPHA  ->  lerp(PRIM, TEXEL0, SHADE_ALPHA)
//     cycle 2: G_CC_BLENDI_SHADE      ->  lerp(COMBINED, ENV, SHADE)
//
// where the vertex colour is a blend *weight* and the lit colour comes from PRIM
// and ENV. Shading those as texel x vertex-colour, as we did, collapses the model
// towards black — which is exactly what Diddy's plane looked like.
// ---------------------------------------------------------------------------

/**
 * A colour input from one of the 4-bit (a, b) or 3-bit (d) slots. The 4-bit slots
 * encode 0..7 and treat anything from 8 up as zero; the 3-bit d slot puts 1 at 6
 * and zero at 7. Slot 7 of a 4-bit slot is NOISE/K4, neither of which DKR uses,
 * so it lands in the zero default.
 */
static void cc_rgb_in(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    s32 i;

    switch (mux) {
        case G_CCMUX_COMBINED:
            for (i = 0; i < 3; i++) {
                out[i] = comb[i];
            }
            return;
        case G_CCMUX_TEXEL0:
        case G_CCMUX_TEXEL1: // no TEXEL1 yet; white leaves it to the texture unit
        case G_CCMUX_1:
            out[0] = out[1] = out[2] = 1.0f;
            return;
        case G_CCMUX_PRIMITIVE:
            for (i = 0; i < 3; i++) {
                out[i] = sPrimColor[i] / 255.0f;
            }
            return;
        case G_CCMUX_SHADE:
            for (i = 0; i < 3; i++) {
                out[i] = shade[i];
            }
            return;
        case G_CCMUX_ENVIRONMENT:
            for (i = 0; i < 3; i++) {
                out[i] = sEnvColor[i] / 255.0f;
            }
            return;
        default:
            out[0] = out[1] = out[2] = 0.0f;
            return;
    }
}

/**
 * A colour input from the 5-bit multiplier (c) slot, which additionally reaches
 * the alpha registers — that is where ENV_ALPHA and SHADE_ALPHA come from, and
 * both are load-bearing for DKR's lighting.
 */
static void cc_rgb_mul(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    f32 scalar;

    switch (mux) {
        case G_CCMUX_COMBINED_ALPHA:
            scalar = comb[3];
            break;
        case G_CCMUX_TEXEL0_ALPHA:
        case G_CCMUX_TEXEL1_ALPHA:
            scalar = 1.0f;
            break;
        case G_CCMUX_PRIMITIVE_ALPHA:
            scalar = sPrimColor[3] / 255.0f;
            break;
        case G_CCMUX_SHADE_ALPHA:
            scalar = shade[3];
            break;
        case G_CCMUX_ENV_ALPHA:
            scalar = sEnvColor[3] / 255.0f;
            break;
        default:
            // Everything below 7 is a plain colour input; LOD_FRAC and K5 are not
            // used by DKR and fall through cc_rgb_in()'s zero default.
            cc_rgb_in(mux, shade, comb, out);
            return;
    }
    out[0] = out[1] = out[2] = scalar;
}

/** Every alpha slot is 3 bits and they all share one encoding. */
static f32 cc_alpha_in(u32 mux, const f32 shade[4], const f32 comb[4]) {
    switch (mux) {
        case G_ACMUX_COMBINED:
            return comb[3];
        case G_ACMUX_TEXEL0:
        case G_ACMUX_TEXEL1:
        case G_ACMUX_1:
            return 1.0f;
        case G_ACMUX_PRIMITIVE:
            return sPrimColor[3] / 255.0f;
        case G_ACMUX_SHADE:
            return shade[3];
        case G_ACMUX_ENVIRONMENT:
            return sEnvColor[3] / 255.0f;
        default:
            return 0.0f; // G_ACMUX_0
    }
}

static f32 cc_clamp(f32 v) {
    if (v <= 0.0f) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

static u8 clamp_u8(f32 v) {
    return (u8) (cc_clamp(v) * 255.0f);
}

/** One cycle of (a - b) * c + d, colour and alpha on their own muxes. */
static void cc_cycle(u32 a, u32 b, u32 c, u32 d, u32 aA, u32 bA, u32 cA, u32 dA, const f32 shade[4], const f32 in[4],
                     f32 out[4]) {
    f32 va[3], vb[3], vc[3], vd[3];
    s32 i;

    cc_rgb_in(a, shade, in, va);
    cc_rgb_in(b, shade, in, vb);
    cc_rgb_mul(c, shade, in, vc);
    cc_rgb_in(d, shade, in, vd);

    for (i = 0; i < 3; i++) {
        out[i] = cc_clamp(((va[i] - vb[i]) * vc[i]) + vd[i]);
    }
    out[3] = cc_clamp(((cc_alpha_in(aA, shade, in) - cc_alpha_in(bA, shade, in)) * cc_alpha_in(cA, shade, in)) +
                      cc_alpha_in(dA, shade, in));
}

/**
 * Run the combiner for one shade value. Both cycles, if othermode says so — the
 * directional-lighting materials are two-cycle and the second cycle is where the
 * environment blend lives, so stopping after cycle 0 (as we used to) throws away
 * the half that matters.
 */
static void combiner_eval(const f32 shade[4], u8 out[4]) {
    f32 cycle0[4];
    f32 result[4];
    s32 i;

    cc_cycle((sCombineW0 >> 20) & 0xF, (sCombineW1 >> 28) & 0xF, (sCombineW0 >> 15) & 0x1F, (sCombineW1 >> 15) & 0x7,
             (sCombineW0 >> 12) & 0x7, (sCombineW1 >> 12) & 0x7, (sCombineW0 >> 9) & 0x7, (sCombineW1 >> 9) & 0x7,
             shade, shade, cycle0);

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) {
        cc_cycle((sCombineW0 >> 5) & 0xF, (sCombineW1 >> 24) & 0xF, sCombineW0 & 0x1F, (sCombineW1 >> 6) & 0x7,
                 (sCombineW1 >> 21) & 0x7, (sCombineW1 >> 3) & 0x7, (sCombineW1 >> 18) & 0x7, sCombineW1 & 0x7, shade,
                 cycle0, result);
    } else {
        for (i = 0; i < 4; i++) {
            result[i] = cycle0[i];
        }
    }

    for (i = 0; i < 4; i++) {
        out[i] = clamp_u8(result[i]);
    }
}

/**
 * The colour a rectangle's vertices carry. A rectangle has no shade, so it goes
 * in as white and the combiner collapses to the constant the texture is modulated
 * by: MODULATEIA_PRIM yields the prim colour, ENVIRONMENT the env colour, and the
 * font's BLENDT_ENV_ALPHA_A_TxP the text colour it keeps in env.
 */
static void combiner_color(u8 out[4]) {
    static const f32 white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    combiner_eval(white, out);
}

/**
 * Two triangles in screen space, which is all a rectangle is once the RDP is out
 * of the picture. Depth testing is off: the game clears G_ZBUFFER for its 2D and
 * relies on display-list order, and so do we.
 */
static void draw_2d_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, const u8 color[4],
                         u32 texture) {
    GfxTriVert q[6];
    s32 i;

    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x1; q[2].y = y1; q[2].u = u1; q[2].v = v1;
    q[3].x = x0; q[3].y = y0; q[3].u = u0; q[3].v = v0;
    q[4].x = x1; q[4].y = y1; q[4].u = u1; q[4].v = v1;
    q[5].x = x0; q[5].y = y1; q[5].u = u0; q[5].v = v1;

    for (i = 0; i < 6; i++) {
        q[i].z = 0.0f;
        q[i].w = 1.0f; // already in screen space — nothing to undo
        q[i].r = color[0];
        q[i].g = color[1];
        q[i].b = color[2];
        q[i].a = color[3];
    }

    // The 2D layer is ordered by the display list, not by depth. Everything the
    // last material batch left set has to be undone; the next G_TRIN re-applies
    // its own state through apply_render_mode(), so nothing needs restoring.
    gfx_set_depth_test(FALSE);
    gfx_set_depth_write(FALSE);
    gfx_set_depth_offset(FALSE);
    gfx_set_alpha_test(0.0f);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_draw_tris(q, 6);
}

/**
 * G_TEXRECT / G_TEXRECTFLIP — the whole 2D layer: every glyph, HUD sprite and
 * menu image. `w2`/`w3` are the two G_RDPHALF words that follow the opcode and
 * carry the texture coordinates.
 */
static void handle_texrect(u32 w0, u32 w1, u32 w2, u32 w3, s32 flip) {
    // Screen coordinates are 10.2 fixed point, texture coordinates S10.5, and
    // the per-pixel texture steps S5.10.
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    f32 s0 = (f32) (s16) (w2 >> 16) / 32.0f;
    f32 t0 = (f32) (s16) (w2 & 0xFFFF) / 32.0f;
    f32 dsdx = (f32) (s16) (w3 >> 16) / 1024.0f;
    f32 dtdy = (f32) (s16) (w3 & 0xFFFF) / 1024.0f;
    f32 s1, t1;
    u8 color[4];
    u32 texture = texture_current();

    if (texture == 0) {
        return;
    }

    // A flipped rect walks s down the rectangle's height and t across its width.
    if (flip) {
        s1 = s0 + ((y1 - y0) * dsdx);
        t1 = t0 + ((x1 - x0) * dtdy);
    } else {
        s1 = s0 + ((x1 - x0) * dsdx);
        t1 = t0 + ((y1 - y0) * dtdy);
    }

    // The texel coordinates are relative to the image, but the texture we
    // uploaded starts at the tile's upper-left corner.
    s0 -= sTileUls / 4.0f;
    s1 -= sTileUls / 4.0f;
    t0 -= sTileUlt / 4.0f;
    t1 -= sTileUlt / 4.0f;

    combiner_color(color);
    draw_2d_quad(x0, y0, x1, y1, s0 / sTileWidth, t0 / sTileHeight, s1 / sTileWidth, t1 / sTileHeight, color,
                 texture);
}

/**
 * G_FILLRECT — the letterbox bars, dialogue-box backgrounds and screen fades.
 */
static void handle_fillrect(u32 w0, u32 w1) {
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    u8 color[4];

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_FILL) {
        // Fill mode paints the fill colour flat, and its lower-right corner is
        // inclusive — one pixel further than the same rect in 1-cycle mode. The
        // colour's alpha bit is the framebuffer's coverage bit, not
        // transparency: the rect is opaque whichever way it is set.
        color[0] = sFillColor[0];
        color[1] = sFillColor[1];
        color[2] = sFillColor[2];
        color[3] = 0xFF;
        x1 += 1.0f;
        y1 += 1.0f;
    } else {
        combiner_color(color);
        if (color[3] == 0) {
            return; // fully transparent — the blender would discard it anyway
        }
    }

    draw_2d_quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, color, 0);
}

/**
 * Interpret one display list. A `count` above zero means "at most this many
 * commands" — the task's own display list is bounded by its length rather than
 * a G_ENDDL, and G_DMADL likewise DMAs a fixed-size block. Zero means "run
 * until G_ENDDL", which is how nested G_DL lists terminate.
 */
static void run_dl(const Gfx *dl, s32 count, s32 depth) {
    s32 i;

    if (dl == NULL || depth > GFX_MAX_DEPTH) {
        return;
    }

    for (i = 0; count == 0 || i < count; i++) {
        u32 w0 = dl[i].words.w0;
        u32 w1 = dl[i].words.w1;
        u32 cmd = (w0 >> 24) & 0xFF;

        // The immediate-mode opcodes are negative ints in gbi.h (G_ENDDL is
        // G_IMMFIRST-7 = -72), so every case has to be masked to the byte the
        // display list actually holds.
        switch (cmd) {
            case G_MTX: {
                s32 slot = (((w0 >> 16) & 0xFF) >> 6) & 3;
                if (w1 != 0 && slot < 3) {
                    mtx_to_float((const Mtx *) w1, sMatrices[slot]);
                    sCurMatrix = slot;
                }
                break;
            }
            case (u8) G_MOVEWORD: {
                u32 index = w0 & 0xFF;
                if (index == G_MW_MVPMATRIX) {
                    sCurMatrix = (w1 >> 6) & 3;
                } else if (index == G_MW_BILLBOARD) {
                    sBillboard = (w1 != 0);
                }
                break;
            }
            case G_MOVEMEM: {
                if ((w0 & 0xFF) == G_MV_VIEWPORT && w1 != 0) {
                    const Vp *vp = (const Vp *) w1;
                    sVpScaleX = vp->vp.vscale[0] / 4.0f;
                    sVpScaleY = vp->vp.vscale[1] / 4.0f;
                    sVpTransX = vp->vp.vtrans[0] / 4.0f;
                    sVpTransY = vp->vp.vtrans[1] / 4.0f;
                }
                break;
            }
            case G_VTX:
                handle_vertex(w0, w1);
                break;
            case G_TRIN:
                handle_polygon(w0, w1);
                break;
            case G_DL:
                run_dl((const Gfx *) w1, 0, depth + 1);
                if (((w0 >> 16) & 0xFF) == G_DL_NOPUSH) {
                    return; // a branch, not a call
                }
                break;
            case G_DMADL:
                run_dl((const Gfx *) w1, (w0 >> 16) & 0xFF, depth + 1);
                break;
            case (u8) G_SETTIMG:
                sTimgAddr = w1;
                break;
            case (u8) G_SETTILE: {
                // Only the tile the triangles actually render with matters.
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    sTileFmt = (w0 >> 21) & 0x7;
                    sTileSiz = (w0 >> 19) & 0x3;
                    sTilePalette = (w1 >> 20) & 0xF;
                    // cmt/cms are 2-bit fields (bit 0 mirror, bit 1 clamp), not
                    // flags. Reading only the low bit picked up mirror and
                    // dropped clamp entirely, so every clamped texture wrapped.
                    sTileCmT = (w1 >> 18) & 0x3;
                    sTileCmS = (w1 >> 8) & 0x3;
                }
                break;
            }
            case (u8) G_SETTILESIZE: {
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    // uls/ult/lrs/lrt are 10.2 fixed point, inclusive bounds.
                    u32 uls = (w0 >> 12) & 0xFFF;
                    u32 ult = w0 & 0xFFF;
                    u32 lrs = (w1 >> 12) & 0xFFF;
                    u32 lrt = w1 & 0xFFF;
                    sTileWidth = ((lrs - uls) >> 2) + 1;
                    sTileHeight = ((lrt - ult) >> 2) + 1;
                    sTileUls = uls;
                    sTileUlt = ult;
                }
                break;
            }
            case (u8) G_LOADBLOCK:
                // Latch the image being loaded: gDPLoadTLUT_pal16 issues its own
                // G_SETTIMG for the palette afterwards, which would otherwise
                // overwrite the texture address before the triangles draw.
                sTexAddr = sTimgAddr;
                // dxt is the low 12 bits. Zero means the RDP applied no odd-row
                // swizzle on the way in, i.e. the asset is already swizzled.
                sTexSwapped = ((w1 & 0xFFF) == 0);
                break;
            case (u8) G_LOADTILE:
                sTexAddr = sTimgAddr;
                sTexSwapped = FALSE; // a tile load is row-by-row, never swizzled
                break;
            case (u8) G_LOADTLUT:
                // The palette is whatever G_SETTIMG last pointed at.
                sTlutAddr = sTimgAddr;
                break;
            case (u8) G_TEXRECT:
            case (u8) G_TEXRECTFLIP: {
                // A texture rectangle is three display-list words: the opcode,
                // then G_RDPHALF_1 and G_RDPHALF_2 carrying the texture
                // coordinates. Skip past them so they are not run as commands.
                if (count != 0 && i + 2 >= count) {
                    return; // truncated — the halves are not there to read
                }
                handle_texrect(w0, w1, dl[i + 1].words.w1, dl[i + 2].words.w1, cmd == (u8) G_TEXRECTFLIP);
                i += 2;
                break;
            }
            case (u8) G_FILLRECT:
                handle_fillrect(w0, w1);
                break;
            case (u8) G_SETSCISSOR: {
                // 10.2 fixed point, lower-right inclusive. This is what confines
                // each player to their own half in split-screen — the viewport
                // only scales and offsets, it does not clip — and what clips the
                // dialogue box's scrolling text reveal (src/font.c).
                f32 x0 = ((w0 >> 12) & 0xFFF) / 4.0f;
                f32 y0 = (w0 & 0xFFF) / 4.0f;
                f32 x1 = ((w1 >> 12) & 0xFFF) / 4.0f;
                f32 y1 = (w1 & 0xFFF) / 4.0f;

                gfx_set_scissor(x0, y0, x1, y1);
                break;
            }
            case (u8) G_SETCOMBINE:
                sCombineW0 = w0;
                sCombineW1 = w1;
                break;
            case (u8) G_SETPRIMCOLOR:
                sPrimColor[0] = (w1 >> 24) & 0xFF;
                sPrimColor[1] = (w1 >> 16) & 0xFF;
                sPrimColor[2] = (w1 >> 8) & 0xFF;
                sPrimColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETENVCOLOR:
                sEnvColor[0] = (w1 >> 24) & 0xFF;
                sEnvColor[1] = (w1 >> 16) & 0xFF;
                sEnvColor[2] = (w1 >> 8) & 0xFF;
                sEnvColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETFILLCOLOR: {
                // Two packed RGBA5551 texels, one per 16-bit half of the word;
                // the game always sets both to the same colour.
                u16 texel = w1 & 0xFFFF;
                u32 rgba = rgba16_to_rgba32(texel);
                sFillColor[0] = rgba & 0xFF;
                sFillColor[1] = (rgba >> 8) & 0xFF;
                sFillColor[2] = (rgba >> 16) & 0xFF;
                sFillColor[3] = (rgba >> 24) & 0xFF;
                break;
            }
            case (u8) G_SETOTHERMODE_H:
            case (u8) G_SETOTHERMODE_L: {
                // F3D encodes the field as a shift and a bit count, with the
                // data already sitting at its final position.
                u32 shift = (w0 >> 8) & 0xFF;
                u32 length = w0 & 0xFF;
                u32 mask = (length >= 32) ? 0xFFFFFFFF : (((u32) 1 << length) - 1) << shift;

                if (cmd == (u8) G_SETOTHERMODE_H) {
                    sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
                } else {
                    sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
                }
                break;
            }
            case (u8) G_RDPSETOTHERMODE:
                // Both halves at once: the high word is packed into w0, the low
                // word — render mode, alpha compare, z mode — is all of w1. This
                // is the form every DKR material arrives in (gsDPSetOtherMode).
                sOtherModeH = w0 & 0x00FFFFFF;
                sOtherModeL = w1;
                break;
            case (u8) G_SETBLENDCOLOR:
                // The alpha is the reference value a G_AC_THRESHOLD compare uses.
                sBlendColor[0] = (w1 >> 24) & 0xFF;
                sBlendColor[1] = (w1 >> 16) & 0xFF;
                sBlendColor[2] = (w1 >> 8) & 0xFF;
                sBlendColor[3] = w1 & 0xFF;
                break;
            case (u8) G_ENDDL:
                return;
            default:
                // Everything else is RDP state we do not model (blenders,
                // scissors, texture filters) plus the G_RDPHALF words consumed
                // by G_TEXRECT above.
                break;
        }
    }
}

void pc_gfx_task_submit(void *dlBegin, void *dlEnd) {
    sGfxFrameCount++;

    gfx_frame_begin();

    sCurMatrix = 0;
    sBillboard = FALSE;
    sVertexBase = 0;
    sTriVertCount = 0;
    sTimgAddr = 0;
    sTexAddr = 0;
    sTexSwapped = 0;
    sTlutAddr = 0;
    sTileWidth = 0;
    sTileHeight = 0;
    sTileUls = 0;
    sTileUlt = 0;
    sOtherModeL = 0;
    // The task's own display list sets a full-screen scissor before it draws
    // anything (src/rcp_dkr.c), but don't inherit last frame's rect until it does.
    gfx_disable_scissor();
    run_dl((const Gfx *) dlBegin, (const Gfx *) dlEnd - (const Gfx *) dlBegin, 0);

    gfx_frame_end();
}

int main(int argc, char **argv) {
    printf("=== DKR PC ===\n");
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
    gfx_window_init(N64_SCREEN_W, N64_SCREEN_H, WINDOW_SCALE);
    thread3_main(0);
    return 0;
}
