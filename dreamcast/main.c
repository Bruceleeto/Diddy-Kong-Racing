#include <stdio.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>
#include <time.h>
#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "gfx.h"

// KOS uptime in ns (dreamcast/reimpl.c). Declared here rather than via
// <kos/timer.h>, whose arch chain redefines R4300.h's EXC_CODE.
extern u64 pc_uptime_ns(void);

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread.
void thread3_main(void *unused);

extern OSThread *__osRunningThread;
static OSThread sHostThread;

#define PC_RETRACE_NSEC (1000000000ll / 60)
#define PC_LOGIC_UPDATE_RATE 2 // LOGIC_30FPS — what gVideoDeltaTime commits to
#define PC_MAX_UPDATE_RATE 6   // don't let a debugger pause become a huge skip

s32 pc_retrace_wait(void) {
    static u64 sLastNs = 0;
    u64 targetNs;
    u64 nowNs;
    s32 periods;

    nowNs = pc_uptime_ns();

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

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define WINDOW_SCALE 3

#define GFX_MAX_VERTS 64    // the RSP's internal vertex array
#define GFX_MAX_DEPTH 16    // display-list recursion guard
#define GFX_MAX_TRI_VERTS 8192

// A vertex in clip space — post-matrix, pre-divide. Kept unprojected because
// near-plane clipping has to interpolate here, before the divide by w (a vertex
// behind the camera has w <= 0, and dividing by it is exactly the garbage the
// clip exists to prevent).
// Field order is deliberate: everything project() reads on the common path is
// packed at the front, so a vertex costs one 32-byte line to shade instead of
// picking fields out of two. clip[] is only touched when a triangle actually
// crosses the near plane, so it goes at the back with the other cold data.
typedef struct {
    // The perspective divide + viewport map, computed once at G_VTX time rather
    // than three-times-per-triangle in push_tri. This is where the RSP does it
    // too: it transforms and projects the vertex when the vertex command loads
    // it, and the triangle commands afterwards only reference the result. Since
    // display lists reuse each vertex across two or three triangles, doing it
    // here rather than per-corner is a straight win.
    // `sw` doubles as the "is this projected" flag, which is what gets the hot
    // set to exactly 32 bytes rather than 33. A projected vertex always has
    // sw == clip[3] >= GFX_NEAR_W (1.0f), so zero is a value it can never
    // legitimately take, and project_into/clip_edge store 0.0f to mean "behind
    // the near plane, nothing cached". Use VERTEX_PROJECTED() to read it.
    f32 sx, sy, sz, sw, sfog; // 20
    f32 u, v;                 // 28 — per-triangle, written by handle_polygon
    u8 r, g, b, a;            // 32 — shade

    f32 clip[4]; // cold: only the clip path reads this
} __attribute__((aligned(32))) GfxVertex;


// Clip anything closer than this. The N64 clips against w, and w is the
// camera-space depth, so this is the near plane in world units.
#define GFX_NEAR_W 1.0f

// Whether a vertex carries a cached projection. See GfxVertex.sw.
#define VERTEX_PROJECTED(v) ((v)->sw != 0.0f)

// Which screen-space winding is a front face. Screen y runs downwards here, so
// this is the opposite sign from the y-up convention. If the world renders
// inside-out — outer surfaces gone, inner ones visible — flip this.
#define GFX_FRONT_FACE_SIGN (-1.0f)

static u32 sGfxFrameCount = 0;

alignas(32) static f32 sMatrices[3][4][4]; // G_MTX_DKR_INDEX_0..2
static s32 sCurMatrix = 0;
static s32 sBillboard = FALSE;

// Layout is load-bearing — see GfxVertex. Assert it rather than trust it.
_Static_assert(sizeof(GfxTriVert) == 32, "GfxTriVert must be exactly one SH4 cache line");
_Static_assert(__builtin_offsetof(GfxVertex, clip) == 32, "GfxVertex hot fields must fit one cache line");

static GfxVertex sVerts[GFX_MAX_VERTS];
static s32 sVertexBase = 0; // where G_VTX_APPEND vertices land

static f32 sVpScaleX = N64_SCREEN_W / 2.0f;
static f32 sVpScaleY = N64_SCREEN_H / 2.0f;
static f32 sVpTransX = N64_SCREEN_W / 2.0f;
static f32 sVpTransY = N64_SCREEN_H / 2.0f;

// ---------------------------------------------------------------------------
// What gets clipped away, in screen pixels, x1/y1 exclusive.
//
// Two rectangles, and the drawing is confined to the intersection:
//
//   sScis*  — the RDP scissor (G_SETSCISSOR). Split-screen, text boxes.
//   sVpClip* — the viewport's own bounds. The RSP clips geometry to the view
//       volume, and the viewport maps NDC +-1 onto exactly this rectangle, so on
//       hardware nothing can be drawn outside it. We only clip against the near
//       plane, so without this a small viewport — the track-preview window in the
//       level-select menu — lets its geometry spill out across the whole screen.
// ---------------------------------------------------------------------------
static f32 sScisX0, sScisY0, sScisX1, sScisY1;
static f32 sVpClipX0, sVpClipY0, sVpClipX1, sVpClipY1;

static f32 max_f(f32 a, f32 b) {
    return (a > b) ? a : b;
}

static f32 min_f(f32 a, f32 b) {
    return (a < b) ? a : b;
}

static void apply_clip_rect(void) {
    gfx_set_scissor(max_f(sScisX0, sVpClipX0), max_f(sScisY0, sVpClipY0), min_f(sScisX1, sVpClipX1),
                    min_f(sScisY1, sVpClipY1));
}

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

#define GFX_GEOMETRY_MODE_INIT (G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER)
static u32 sGeometryMode = GFX_GEOMETRY_MODE_INIT;

static u32 sOtherModeH = 0;      // G_SETOTHERMODE_H / G_RDPSETOTHERMODE
static u32 sOtherModeL = 0;      // G_SETOTHERMODE_L / G_RDPSETOTHERMODE
static u32 sCombineW0 = 0;       // G_SETCOMBINE — the two mux words
static u32 sCombineW1 = 0;
static u8 sPrimColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sEnvColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sFillColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static u8 sBlendColor[4] = { 0x00, 0x00, 0x00, 0xFF }; // G_SETBLENDCOLOR

// Fog. The game recomputes this every frame, per player (src/tracks.c), out of the
// FogData system its fog-changer objects drive. gSPFogPosition packs a multiplier
// and an offset into one G_MOVEWORD, and the RSP turns them into a per-vertex fade
// factor; gDPSetFogColor is the colour that factor fades towards. 
static u8 sFogColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static s16 sFogMul = 0;
static s16 sFogOfs = 0;

#define GFX_MAX_TEXTURES 1024
#define GFX_MAX_TEX_TEXELS (256 * 256)

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
static void project_into(GfxVertex *v);

static void load_vertex(GfxVertex *dst, const Vertex *v, const f32 *anchor) {
    SHZ_ALIASING shz_vec4_t* out = (SHZ_ALIASING shz_vec4_t*)dst->clip;

    *out = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));

    if(anchor)
        *out = shz_vec4_add(*out, *(SHZ_ALIASING shz_vec4_t*)anchor);

    dst->r = v->r;
    dst->g = v->g;
    dst->b = v->b;
    // Shade alpha. The combiner uses it as a blend weight (SHADE_ALPHA), and the
    // RENDER_VTX_ALPHA materials use it as real translucency — which is why it is
    // mutually exclusive with fog in material_set(): the RSP keeps vertex alpha in
    // the fog slot.
    dst->a = v->a;

    project_into(dst);
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

    shz_xmtrx_load_4x4((shz_mat4x4_t*)&sMatrices[sCurMatrix]);
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
static void combiner_eval(const f32 shade[4], u8 out[4], u8 sec[3]);
static void combiner_eval_lit(const f32 shade[4], u8 lit[4]);
static void combiner_classify(void);
static u8 clamp_u8(f32 v);

// How the current batch's combiner collapses, per channel; see
// combiner_classify(). sCcFast means every channel is either passthrough or a
// constant, so project() can skip the mux entirely.
static u8 sCcPass[4];  // 1 = this channel is the shade, unchanged
static u8 sCcConst[4]; // otherwise, the constant it always produces
static s32 sCcFast;

/**
 * The geometric half: perspective divide, viewport map and fog. Depends only on
 * RSP state (matrices, viewport, fog registers, geometry mode), all of which is
 * settled by the time the vertex is loaded — so this is what gets hoisted to
 * G_VTX time and cached in the GfxVertex.
 *
 * w is guaranteed >= GFX_NEAR_W by every caller, which is what makes the fsrra
 * reciprocal safe (it is defined for positive inputs only).
 */
static void project_geom(const GfxVertex *v, f32 *sx, f32 *sy, f32 *sz, f32 *sw, f32 *sfog) {
    f32 invW = shz_invf_fsrra(v->clip[3]);
    f32 ndcZ = v->clip[2] * invW;

    *sx = (v->clip[0] * invW * sVpScaleX) + sVpTransX;
    *sy = sVpTransY - (v->clip[1] * invW * sVpScaleY);
    // Negated so nearer geometry gets the smaller depth under GL_LESS.
    *sz = -ndcZ;
    // Kept so the host layer can restore the homogeneous position and get
    // perspective-correct texturing out of the fixed-function pipeline. The near
    // clip guarantees this is >= GFX_NEAR_W, so it is safe to multiply back by.
    *sw = v->clip[3];

    // Fog: the RSP's own formula, ndc_z * mul + ofs, clamped to a byte. The
    // geometry mode gates it — material_set() sets and clears G_FOG per material,
    // and clears it whenever a material wants vertex alpha instead, because the
    // RSP keeps the fog factor in the shade-alpha slot.
    if (sGeometryMode & G_FOG) {
        f32 fog = (ndcZ * (f32) sFogMul) + (f32) sFogOfs;

        if (fog < 0.0f) {
            fog = 0.0f;
        }
        if (fog > 255.0f) {
            fog = 255.0f;
        }
        *sfog = fog * (1.0f / 255.0f);
    } else {
        *sfog = 0.0f;
    }
}

/** Cache the projection into the vertex, or mark it as behind the near plane. */
static void project_into(GfxVertex *v) {
    if (v->clip[3] >= GFX_NEAR_W) {
        project_geom(v, &v->sx, &v->sy, &v->sz, &v->sw, &v->sfog);
    } else {
        v->sw = 0.0f; // behind the near plane — nothing cached
    }
}

/**
 * Turn a clip-space vertex plus its per-triangle UVs into an output vertex.
 * Takes the cached projection when there is one; only vertices manufactured by
 * clip_edge have to be projected here.
 *
 * The combiner is deliberately *not* hoisted with the geometry: it is RDP state,
 * not RSP state, so it can legitimately change between the G_VTX that loads a
 * vertex and the G_TRIN that draws with it.
 */
static void project(const GfxVertex *v, GfxTriVert *out) {
    if (VERTEX_PROJECTED(v)) {
        out->x = v->sx;
        out->y = v->sy;
        out->z = v->sz;
        out->w = v->sw;
        out->fog = v->sfog;
    } else {
        project_geom(v, &out->x, &out->y, &out->z, &out->w, &out->fog);
    }
    out->u = v->u;
    out->v = v->v;

    // The colour the RDP would have computed *around* the texel: the environment
    // blend, the prim colour, and the prim/vertex alpha behind every fade in the
    // game. The texture unit modulates the real texel in afterwards, so the
    // combiner runs here with the texel taken as white.
    //
    // Which of the two paths below applies was settled once for the whole batch;
    // see combiner_classify().
    if (sCcFast) {
        // Every channel is either the shade straight through or a per-batch
        // constant, so the whole mux collapses to four selects. Passthrough is
        // spelled byte -> float -> clamp_u8, the same round trip the general path
        // makes, so the quantisation is identical rather than merely close.
        out->r = sCcPass[0] ? clamp_u8(v->r * (1.0f / 255.0f)) : sCcConst[0];
        out->g = sCcPass[1] ? clamp_u8(v->g * (1.0f / 255.0f)) : sCcConst[1];
        out->b = sCcPass[2] ? clamp_u8(v->b * (1.0f / 255.0f)) : sCcConst[2];
        out->a = sCcPass[3] ? clamp_u8(v->a * (1.0f / 255.0f)) : sCcConst[3];
    } else {
        // At least one channel genuinely mixes shade with something else. Run the
        // real thing. Only the lit half is wanted — see combiner_eval_lit.
        f32 shade[4];
        u8 lit[4];

        shade[0] = v->r * (1.0f / 255.0f);
        shade[1] = v->g * (1.0f / 255.0f);
        shade[2] = v->b * (1.0f / 255.0f);
        shade[3] = v->a * (1.0f / 255.0f);
        combiner_eval_lit(shade, lit);

        out->r = lit[0];
        out->g = lit[1];
        out->b = lit[2];
        out->a = lit[3];
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
    f32 t = shz_divf(GFX_NEAR_W - a->clip[3], b->clip[3] - a->clip[3]);
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

    // Brand new clip-space position — nothing cached applies to it. project()
    // will compute the projection for this one on the spot.
    out->sw = 0.0f;
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

    // Nothing crosses the near plane, which is the overwhelmingly common case:
    // emit straight from the loaded vertices instead of staging the triangle
    // through poly[]. A cached projection exists exactly when
    // clip[3] >= GFX_NEAR_W, so this is the same test the loop below makes.
    if (VERTEX_PROJECTED(v0) && VERTEX_PROJECTED(v1) && VERTEX_PROJECTED(v2)) {
        push_tri(v0, v1, v2, cull);
        return;
    }

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


static void apply_texture_filter(void) {
    u32 filt = sOtherModeH & (3 << G_MDSFT_TEXTFILT);

    gfx_set_texture_filter(filt == G_TF_POINT);
}

static void apply_render_mode(void) {
    u32 alphaCompare = sOtherModeL & (3 << G_MDSFT_ALPHACOMPARE);
    s32 zEnabled = (sGeometryMode & G_ZBUFFER) != 0;
    f32 ref = 0.0f;

    // Depth needs both halves to agree: the RSP has to be emitting z (geometry
    // mode) and the RDP has to be told to use it (render mode). Clearing G_ZBUFFER
    // is how the game turns depth off for the sky, overlays and its 2D layer.
    gfx_set_depth_test(zEnabled && (sOtherModeL & Z_CMP) != 0);
    gfx_set_depth_write(zEnabled && (sOtherModeL & Z_UPD) != 0);
    gfx_set_depth_offset(zEnabled && (sOtherModeL & ZMODE_DEC) == ZMODE_DEC);

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

        invTexW = shz_invf_fsrra(32.0f * (f32) sTileWidth);
        invTexH = shz_invf_fsrra(32.0f * (f32) sTileHeight);
    }

    // Each G_TRIN is one material batch, so it becomes one draw call.
    sTriVertCount = 0;

    // Settle how this batch's combiner collapses before touching any vertex. The
    // mux, prim and env cannot change inside a batch, so once is enough.
    combiner_classify();

    for (i = 0; i < count; i++) {
        // The UVs are the only per-triangle part of a vertex, so write them into
        // the loaded vertices in place and pass those along, rather than taking a
        // copy of each GfxVertex per corner. The copy only existed to carry the
        // UVs, and it costs more now that the struct also caches the projection.
        // Overwriting is safe because emit_triangle consumes them before the next
        // iteration touches them again.
        GfxVertex *a = &sVerts[tris[i].vi0 % GFX_MAX_VERTS];
        GfxVertex *b = &sVerts[tris[i].vi1 % GFX_MAX_VERTS];
        GfxVertex *c = &sVerts[tris[i].vi2 % GFX_MAX_VERTS];

        a->u = tris[i].uv0.u * invTexW;
        a->v = tris[i].uv0.v * invTexH;
        b->u = tris[i].uv1.u * invTexW;
        b->v = tris[i].uv1.v * invTexH;
        c->u = tris[i].uv2.u * invTexW;
        c->v = tris[i].uv2.v * invTexH;

        // DKR marks culling per triangle: BACKFACE_DRAW means double-sided.
        emit_triangle(a, b, c, !(tris[i].flags & BACKFACE_DRAW));
    }

    apply_render_mode();
    gfx_set_fog((sGeometryMode & G_FOG) != 0, sFogColor);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_set_texenv_modulate(); // the 2D path leaves the env in blend mode
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
static f32 sCcTexel = 1.0f; // the value TEXEL0/TEXEL1 take for this evaluation

static void cc_rgb_in(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    s32 i;

    switch (mux) {
        case G_CCMUX_COMBINED:
            for (i = 0; i < 3; i++) {
                out[i] = comb[i];
            }
            return;
        case G_CCMUX_TEXEL0:
        case G_CCMUX_TEXEL1: // no TEXEL1 yet; it follows TEXEL0
            out[0] = out[1] = out[2] = sCcTexel;
            return;
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
            scalar = sCcTexel;
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
            return sCcTexel;
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
static void combiner_run(const f32 shade[4], f32 out[4]) {
    f32 cycle0[4];
    s32 i;

    cc_cycle((sCombineW0 >> 20) & 0xF, (sCombineW1 >> 28) & 0xF, (sCombineW0 >> 15) & 0x1F, (sCombineW1 >> 15) & 0x7,
             (sCombineW0 >> 12) & 0x7, (sCombineW1 >> 12) & 0x7, (sCombineW0 >> 9) & 0x7, (sCombineW1 >> 9) & 0x7,
             shade, shade, cycle0);

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) {
        cc_cycle((sCombineW0 >> 5) & 0xF, (sCombineW1 >> 24) & 0xF, sCombineW0 & 0x1F, (sCombineW1 >> 6) & 0x7,
                 (sCombineW1 >> 21) & 0x7, (sCombineW1 >> 3) & 0x7, (sCombineW1 >> 18) & 0x7, sCombineW1 & 0x7, shade,
                 cycle0, out);
    } else {
        for (i = 0; i < 4; i++) {
            out[i] = cycle0[i];
        }
    }
}

/**
 * Split the combiner into the two halves the fixed-function pipeline can apply:
 * the factor the texel is multiplied by, and the term added to it afterwards.
 *
 * Every combiner DKR uses is affine in TEXEL0 — result = TEXEL0 * K1 + K2 — so
 * running it twice, once with the texel white and once with it black, recovers
 * both halves: K2 is the result with no texel at all, and K1 is what the texel
 * added on top. GL then reproduces it exactly: K1 goes in the vertex colour and
 * modulates the texture, K2 in the secondary colour, which GL_COLOR_SUM adds
 * after texturing.
 *
 * This is what makes the menu's flashing highlight work. The font combiner is
 * G_CC_BLENDT_ENV_ALPHA_A_TxP — lerp(TEXEL0, ENV, ENV_ALPHA) — and font.c puts the
 * highlight colour in ENV and its pulse in ENV's alpha. Folding everything into a
 * single modulated colour, as we used to, computes texel * ENV: at full blend that
 * is texel * white, i.e. the original texel, and the flash silently cancels out.
 *
 * Alpha stays a plain modulate (GL_COLOR_SUM is RGB-only), which is exact for the
 * TEXEL0 * PRIM form DKR's 2D uses everywhere.
 */
static void combiner_eval(const f32 shade[4], u8 lit[4], u8 unlit[3]) {
    f32 on[4];  // texel = white
    f32 off[4]; // texel = black
    s32 i;

    sCcTexel = 1.0f;
    combiner_run(shade, on);
    sCcTexel = 0.0f;
    combiner_run(shade, off);
    sCcTexel = 1.0f;

    for (i = 0; i < 3; i++) {
        lit[i] = clamp_u8(on[i]);
        unlit[i] = clamp_u8(off[i]);
    }
    lit[3] = clamp_u8(on[3]);
}

/**
 * Just the lit half. The 3D path (project) only ever consumes `lit` — the
 * texel=0 run that combiner_eval also does is thrown away there, so the second
 * combiner_run was pure waste on every 3D vertex in the frame. The 2D path still
 * needs both halves and keeps using combiner_eval.
 */
static void combiner_eval_lit(const f32 shade[4], u8 lit[4]) {
    f32 on[4];
    s32 i;

    sCcTexel = 1.0f;
    combiner_run(shade, on);

    for (i = 0; i < 4; i++) {
        lit[i] = clamp_u8(on[i]);
    }
}

// ---------------------------------------------------------------------------
// Per-batch combiner classification.
//
// Running the mux per vertex was measured at ~29ms a frame — by far the largest
// single cost in the renderer. But the mux, the prim colour and the env colour
// are all constant for a whole draw batch; only shade varies. So for most
// batches each output channel is one of just two things:
//
//   passthrough — the channel *is* the shade. A plain texel * shade modulate,
//                 which is what DKR's 3D geometry uses almost everywhere.
//   constant    — the channel never varies with shade, so evaluate it once.
//
// When every channel is one of those the combiner collapses to four selects and
// need not run at all. Anything else falls back to the real mux, per vertex.
//
// Classifying per channel rather than per batch is the whole point: DKR's 3D
// materials are texel * shade on RGB but take alpha from PRIM or ENV to drive
// fades, so an all-or-nothing test rejects exactly the batches worth catching.
//
// The test is functional, not a mux pattern-match — evaluate the real combiner
// on probe shades and look at what comes out. An unrecognised combiner cannot be
// misclassified; it simply fails both tests and takes the slow path.
// ---------------------------------------------------------------------------
// The state the classification is valid for. Any change and it is recomputed.
static u32 sCcSigW0, sCcSigW1, sCcSigCyc, sCcSigPrim, sCcSigEnv;
static s32 sCcSigValid = FALSE;

static void combiner_classify(void) {
    // Three probes, not two. Two mid-range ones alone would call a combiner that
    // *saturates* at both of them constant — lerp-towards-env clamps to 255 at
    // any bright shade, but not at a dark one — so the third sits on the 0/255
    // extremes, where a saturating combiner gives itself away.
    static const u8 probeA[4] = { 13, 71, 149, 233 };
    static const u8 probeB[4] = { 200, 5, 96, 44 };
    static const u8 probeC[4] = { 0, 255, 255, 0 };
    u32 cyc = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    u32 prim = (sPrimColor[0] << 24) | (sPrimColor[1] << 16) | (sPrimColor[2] << 8) | sPrimColor[3];
    u32 env = (sEnvColor[0] << 24) | (sEnvColor[1] << 16) | (sEnvColor[2] << 8) | sEnvColor[3];
    f32 shade[4];
    u8 a[4], b[4], c[4];
    s32 i;

    if (sCcSigValid && sCombineW0 == sCcSigW0 && sCombineW1 == sCcSigW1 && cyc == sCcSigCyc && prim == sCcSigPrim &&
        env == sCcSigEnv) {
        return; // still the combiner we classified last time
    }
    sCcSigW0 = sCombineW0;
    sCcSigW1 = sCombineW1;
    sCcSigCyc = cyc;
    sCcSigPrim = prim;
    sCcSigEnv = env;
    sCcSigValid = TRUE;

    for (i = 0; i < 4; i++) {
        shade[i] = probeA[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, a);
    for (i = 0; i < 4; i++) {
        shade[i] = probeB[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, b);
    for (i = 0; i < 4; i++) {
        shade[i] = probeC[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, c);

    // Classify each channel on its own. Doing this all-or-nothing was the bug:
    // DKR's 3D materials are texel * shade on RGB but take alpha from PRIM or ENV
    // for fades, so a single non-passthrough channel used to drop the whole batch
    // onto the slow path — which is every batch that matters.
    sCcFast = TRUE;
    for (i = 0; i < 4; i++) {
        if (a[i] == probeA[i] && b[i] == probeB[i] && c[i] == probeC[i]) {
            sCcPass[i] = TRUE; // shade in, same shade out
            sCcConst[i] = 0;
        } else if (a[i] == b[i] && a[i] == c[i]) {
            sCcPass[i] = FALSE; // never varies with shade
            sCcConst[i] = a[i];
        } else {
            sCcFast = FALSE; // genuinely mixes shade with something else
            break;
        }
    }
}

/**
 * The two colours a rectangle's vertices carry. A rectangle has no shade, so it
 * goes in as white and the combiner is constant across the quad — but it still has
 * to be split into its modulate and add halves, because the font blends the glyph
 * towards ENV and that add is the whole point (see combiner_eval).
 */
static void combiner_color(u8 lit[4], u8 unlit[3]) {
    static const f32 white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    combiner_eval(white, lit, unlit);
}

/**
 * Two triangles in screen space, which is all a rectangle is once the RDP is out
 * of the picture. Depth testing is off: the game clears G_ZBUFFER for its 2D and
 * relies on display-list order, and so do we.
 */
static void draw_2d_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, const u8 lit[4],
                         const u8 unlit[3], u32 texture) {
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
        q[i].fog = 0.0f;
        // Textured: the vertex carries the no-texel end of the combiner and the
        // texture env carries the full-texel end, and GL_BLEND lerps between them
        // by the texel — reproducing the combiner exactly. Untextured: there is no
        // texel to lerp with, so the vertex carries the whole result.
        q[i].r = (texture != 0) ? unlit[0] : lit[0];
        q[i].g = (texture != 0) ? unlit[1] : lit[1];
        q[i].b = (texture != 0) ? unlit[2] : lit[2];
        q[i].a = lit[3];
    }

    if (texture != 0) {
        gfx_set_texenv_blend(lit);
    } else {
        gfx_set_texenv_modulate();
    }

    // The 2D layer is ordered by the display list, not by depth. Everything the
    // last material batch left set has to be undone; the next G_TRIN re-applies
    // its own state through apply_render_mode(), so nothing needs restoring.
    gfx_set_depth_test(FALSE);
    gfx_set_depth_write(FALSE);
    gfx_set_depth_offset(FALSE);
    gfx_set_alpha_test(0.0f);
    gfx_set_fog(FALSE, sFogColor); // the HUD does not sit in the world's haze
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
    u8 lit[4];
    u8 unlit[3];
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

    combiner_color(lit, unlit);
    draw_2d_quad(x0, y0, x1, y1, s0 / sTileWidth, t0 / sTileHeight, s1 / sTileWidth, t1 / sTileHeight, lit, unlit,
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
    u8 unlit[3] = { 0, 0, 0 };

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
        combiner_color(color, unlit);
        if (color[3] == 0) {
            return; // fully transparent — the blender would discard it anyway
        }
    }

    draw_2d_quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, color, unlit, 0);
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
                } else if (index == G_MW_FOG) {
                    // gSPFogPosition packs the multiplier in the high half and the
                    // offset in the low half; both are signed.
                    sFogMul = (s16) (w1 >> 16);
                    sFogOfs = (s16) (w1 & 0xFFFF);
                }
                break;
            }
            case G_MOVEMEM: {
                // gSPViewport goes through gDma1p, which packs the parameter at
                // bits 16-23 and the *length* at 0-15 (include/PR/gbi.h). Reading
                // the low byte matched the length, never the parameter, so the
                // viewport was silently ignored: everything rendered at the
                // 320x240 default, which looks right full-screen and falls apart
                // the moment the game asks for a smaller one.
                if (((w0 >> 16) & 0xFF) == G_MV_VIEWPORT && w1 != 0) {
                    const Vp *vp = (const Vp *) w1;
                    f32 halfW, halfH;

                    sVpScaleX = vp->vp.vscale[0] / 4.0f;
                    sVpScaleY = vp->vp.vscale[1] / 4.0f;
                    sVpTransX = vp->vp.vtrans[0] / 4.0f;
                    sVpTransY = vp->vp.vtrans[1] / 4.0f;

                    // The rectangle NDC +-1 maps onto. The scale carries a sign
                    // (y is commonly negative, since screen y runs the other way
                    // from clip y), so take it off before using it as a half-size.
                    halfW = (sVpScaleX < 0.0f) ? -sVpScaleX : sVpScaleX;
                    halfH = (sVpScaleY < 0.0f) ? -sVpScaleY : sVpScaleY;
                    sVpClipX0 = sVpTransX - halfW;
                    sVpClipX1 = sVpTransX + halfW;
                    sVpClipY0 = sVpTransY - halfH;
                    sVpClipY1 = sVpTransY + halfH;
                    apply_clip_rect();
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
                // 10.2 fixed point, lower-right inclusive — a full-screen scissor
                // arrives as (0, 0, 319, 239), hence the +1 to make it exclusive.
                // This is what clips the dialogue box's scrolling text reveal
                // (src/font.c) and, with the viewport, each player's half.
                sScisX0 = ((w0 >> 12) & 0xFFF) / 4.0f;
                sScisY0 = (w0 & 0xFFF) / 4.0f;
                sScisX1 = (((w1 >> 12) & 0xFFF) / 4.0f) + 1.0f;
                sScisY1 = ((w1 & 0xFFF) / 4.0f) + 1.0f;
                apply_clip_rect();
                break;
            }
            case (u8) G_SETGEOMETRYMODE:
                sGeometryMode |= w1;
                break;
            case (u8) G_CLEARGEOMETRYMODE:
                sGeometryMode &= ~w1;
                break;
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
            case (u8) G_SETFOGCOLOR:
                sFogColor[0] = (w1 >> 24) & 0xFF;
                sFogColor[1] = (w1 >> 16) & 0xFF;
                sFogColor[2] = (w1 >> 8) & 0xFF;
                sFogColor[3] = w1 & 0xFF;
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

extern void dc_audio_init(void);
extern void pc_audio_frame(void);
extern void pc_audio_report(void);

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
    sGeometryMode = GFX_GEOMETRY_MODE_INIT;

    // The list sets its own scissor and viewport before it draws anything
    // (src/rcp_dkr.c), but don't inherit last frame's rects until it does.
    sScisX0 = sVpClipX0 = 0.0f;
    sScisY0 = sVpClipY0 = 0.0f;
    sScisX1 = sVpClipX1 = (f32) N64_SCREEN_W;
    sScisY1 = sVpClipY1 = (f32) N64_SCREEN_H;
    gfx_disable_scissor();

    run_dl((const Gfx *) dlBegin, (const Gfx *) dlEnd - (const Gfx *) dlBegin, 0);

    gfx_frame_end();

    // Tick the audio manager once per frame. On N64 this is the scheduler posting
    // OS_SC_RETRACE_MSG to the audio thread; there is no thread and no scheduler
    // here, so the frame boundary drives it directly. (linux/audio.c)
    pc_audio_frame();
    pc_audio_report();
}

static void cont_reset_btn_callback_(uint8_t addr, uint32_t btns) {
    (void)addr;
    (void)btns;
    arch_exit();
}

int main(int argc, char **argv) {
    cont_btn_callback(0, CONT_RESET_BUTTONS, cont_reset_btn_callback_);

    printf("=== DKR PC ===\n");
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
    gfx_window_init(N64_SCREEN_W, N64_SCREEN_H, WINDOW_SCALE);
    // Bring AICA up now, well before init_game produces the first PCM.
    dc_audio_init();
    thread3_main(0);
    return 0;
}