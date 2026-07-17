// Dreamcast graphics backend — KallistiOS PVR. First-pass renderer.
//
// This implements the same gfx.h interface the SDL/OpenGL host renderer does, so
// the F3DDKR display-list interpreter (dreamcast/main.c) is untouched: it still
// hands us screen-space triangles that are already projected and perspective-
// divided, with per-vertex colour, uv and w. All this file does is turn that
// stream into PVR TA submissions.
//
// Design (deliberately simple — "untextured polys are fine" first cut):
//
//   * TWO lists. Everything goes to PVR_LIST_TR_POLY, with hardware autosort
//     DISABLED, except alpha-tested batches (gfx_set_alpha_test with ref > 0),
//     which go to PVR_LIST_PT_POLY. Punch-through is the only PVR path that
//     discards a texel before the depth stage, which is exactly what the RDP's
//     alpha compare does and what the trees need: without it their transparent
//     texels blend away to nothing but still stamp the W-buffer, and everything
//     drawn later and behind them is depth-rejected — a sprite-shaped hole showing
//     whatever was in the framebuffer first.
//
//     The PT threshold is one global register, not per-poly. That is exact here
//     rather than a compromise: every alpha-tested material in the game resolves to
//     ref = 0.5 (CVG_X_ALPHA), and nothing uses G_AC_THRESHOLD.
//
//     A list cannot be reopened once closed, and the TR list is open for the whole
//     display-list walk, so PT batches are recorded into sPtBuf as raw 32-byte TA
//     words and replayed into the PT list at frame end. Submission order between
//     lists does not matter: the PVR always renders OP, then PT, then TR.
//
//   * The TR list, with autosort DISABLED. That makes the
//     PVR honour submission order exactly like GL's immediate mode, so the game's
//     own back-to-front / overlay-last draw order just works. Depth is still
//     resolved per-pixel through the W-buffer (1/w in the vertex z field), so
//     solid geometry occludes correctly; transparency blends in order on top.
//     Routing opaque geometry onto the faster PVR_LIST_OP_POLY is a later
//     optimisation, not needed to see the game.
//
//   * Geometry is fed to the TA through the SH4 store queues (pvr_dr_*), the same
//     way OoT's DC renderer does — a 32-byte header or vertex per store-queue
//     burst, no per-primitive memcpy.
//
//   * The GL fixed-function state (bound texture, depth compare/write, filter,
//     scissor) is mirrored in statics and folded into a PVR poly header that is
//     (re)compiled only when something changed, then submitted ahead of each
//     triangle batch.
//
// Known first-pass gaps, all cosmetic: no fog; the menu-highlight texenv "blend
// toward constant" is approximated by modulate.

#include "gfx.h"

#include <kos.h>
#include <dc/pvr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// State mirrored from the gfx.h setters
// ---------------------------------------------------------------------------

#define DC_SCREEN_W 640
#define DC_SCREEN_H 480

#define MAX_TEXTURES 1024

typedef struct {
    pvr_ptr_t data; // NULL means the slot is free
    int w, h;               // original N64 texture size
    int padded_w, padded_h; // power-of-two size actually stored in VRAM
    float u_scale, v_scale;  // original / padded — folds NPOT padding into the UVs
    int cmS, cmT;
    // Every texel fully opaque. Only such a texture may be routed to the OP list,
    // which has no blend unit — a translucent one sent there renders hard instead
    // of soft. Measured at upload rather than guessed from the vertex colour, which
    // says nothing about what the texture does.
    int opaque;
} DcTexture;

static DcTexture sTextures[MAX_TEXTURES];

static int sPvrReady;
static int sInScene;

// N64 framebuffer size the game draws in, and how we stretch it to fill the DC
// 640x480 output.
static float sScaleX = 2.0f;
static float sScaleY = 2.0f;

// Current render state.
static int sBoundTex;      // 1-based index into sTextures, 0 = untextured
static int sFilterPoint;   // 1 = nearest, 0 = bilinear
// texenv "blend toward constant" (the textured 2D path — font glyphs, logos, and
// the menu highlight). The RDP combiner is linear in the texel:
//     result = unlit + texel * (lit - unlit)
// where `unlit`/`lit` are the combiner evaluated at texel 0 and 1. The interpreter
// hands us `unlit` per-vertex (the vertex colour) and `lit` as this constant. The
// PVR computes texel*argb + oargb (offset colour, added post-modulate), so this
// maps EXACTLY: argb = lit - unlit, oargb = unlit. No approximation — and the
// selection highlight, which lives in the per-vertex `unlit`, survives because it
// rides in oargb.
static int sTexEnvBlend;
static unsigned char sBlendColor[4]; // `lit`
static int sDepthTest = 1;
static int sDepthWrite = 1;
static int sDepthOffset;   // decal bias
static float sAlphaRef;    // > 0 routes the batch to the punch-through list

// Scissor, in N64 pixels (top-left origin). Full screen by default.
static int sScisEnable;
static float sScisX0, sScisY0, sScisX1, sScisY1;

// Recompute-on-demand flags. The scissor needs one per list: the two lists are
// built independently, so a clip consumed while recording PT would never reach TR.
static int sHdrDirty = 1;
static int sScisDirty = 1;
static int sScisDirtyPt = 1;
static int sScisDirtyOp = 1;

static pvr_poly_hdr_t sHdr __attribute__((aligned(32)));
static pvr_dr_state_t sDrState;

// Punch-through recording. Headers, vertices and user-clip commands are all one
// 32-byte TA word, so a batch is recorded as the exact word stream it would have
// been submitted as, and replayed verbatim. 4096 words is ~30x the ~130 a frame of
// trees needs; on overflow the surplus is dropped and reported once.
// PT routing is off. It cuts the trees out correctly — the alpha test fires and the
// silhouette is right — but the PVR renders OP, then PT, then TR in hardware, and
// this backend puts everything else in TR. So a PT tree jumps ahead of the whole
// scene and anything TR draws early (the black fill quad, batch 1) lands on top of
// it. Fixing that means classifying the lists properly (opaque -> OP, cutout -> PT,
// translucent -> TR) the way MK64 does, which is a rewrite of this file's central
// "ONE list" decision, not a switch. The recording path below is kept because it is
// correct and that rewrite would need it.
//
// Until then: see compile_header_for(), which keeps alpha-tested batches out of the
// W-buffer instead.
#define USE_PT_LIST 1

#define PT_MAX_WORDS 4096

static unsigned char sPtBuf[PT_MAX_WORDS][32] __attribute__((aligned(32)));
static int sPtCount;
static int sPtOverflow;

// The backdrop: the screen fills and the skybox. Both are drawn with depth compare
// ALWAYS — the fills are the game's 2D clear, and DKR clears G_ZBUFFER for the sky
// so it can never be depth-rejected. In TR they render *after* the PT list and paint
// straight over the alpha-tested sprites, which is exactly what makes a tree show
// sky through it. They belong in OP, which the PVR renders first. Recorded the same
// way PT is; the sky is a few hundred polys at most.
#define OP_MAX_WORDS 4096

static unsigned char sOpBuf[OP_MAX_WORDS][32] __attribute__((aligned(32)));
static int sOpCount;
static int sOpOverflow;

// VRAM freed while a scene is open. A recorded header holds a raw texture pointer
// and is not replayed until frame end, so freeing mid-frame lets the next upload's
// pvr_mem_malloc hand the same block straight back and the recording ends up drawing
// whatever landed there. The frees are held until after the replay instead. This
// only matters for the recorded lists; TR is submitted as it goes.
#define PENDING_FREE_MAX 512

static pvr_ptr_t sPendingFree[PENDING_FREE_MAX];
static int sPendingFreeCount;

// Where the batch currently being emitted is going.
#define ROUTE_TR 0
#define ROUTE_PT 1
#define ROUTE_OP 2

static int sRoute;

// PVR punch-through alpha threshold: a texel with alpha below this is discarded
// before the depth stage. KOS neither names nor writes this register. 0x80 is the
// RDP's ref = 0.5, which is what every alpha-tested material in the game asks for.
#define PVR_PT_ALPHA_REF 0xA05F811C
#define PT_ALPHA_REF_VALUE 0x80

/** Where the next 32-byte TA word goes: the store queues, or one of the recordings. */
static void *ta_target(void) {
    if (sRoute == ROUTE_PT) {
        return sPtBuf[sPtCount];
    }
    if (sRoute == ROUTE_OP) {
        return sOpBuf[sOpCount];
    }
    return pvr_dr_target(sDrState);
}

static void ta_commit(void *p) {
    if (sRoute == ROUTE_PT) {
        sPtCount++;
    } else if (sRoute == ROUTE_OP) {
        sOpCount++;
    } else {
        pvr_dr_commit(p);
    }
}

// ---------------------------------------------------------------------------
// Init / teardown
// ---------------------------------------------------------------------------

void gfx_window_init(int width, int height, int scale) {
    pvr_init_params_t params = {
        // OP, OP_MOD, TR, TR_MOD, PT. The PT bin has to be open for the
        // alpha-tested batches; it was BINSIZE_0 (disabled) while everything went
        // to TR.
        { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
        // TA vertex buffer. A full track submits thousands of triangles, each as
        // its own 3-vertex strip (~96 bytes); 512K (~5.5k tris) overflows on the
        // heavy tracks and the PVR then hangs at pvr_wait_ready. 1.5M (~16k tris)
        // clears it with VRAM to spare for framebuffers and textures.
        1536 * 1024, // vertex buffer
        0,          // DMA disabled (we submit via the store queues)
        0,          // no FSAA
        1,          // autosort DISABLED -> submission order preserved
        3           // OPB overflow count
    };

    (void) scale; // the DC always outputs 640x480; we stretch to fit

    if (sPvrReady) {
        return;
    }

    sScaleX = (float) DC_SCREEN_W / (float) width;
    sScaleY = (float) DC_SCREEN_H / (float) height;

    vid_set_mode(DM_640x480, PM_RGB565);
    if (pvr_init(&params) < 0) {
        return;
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    // Set after pvr_init so it cannot be clobbered by it.
    *((volatile unsigned int *) PVR_PT_ALPHA_REF) = PT_ALPHA_REF_VALUE;

    sPvrReady = 1;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

// RDP cms/cmt -> PVR clamp. bit1 = clamp (wins), bit0 = mirror. Clamp handled
// here; mirror via uv_flip below.
static int uv_clamp_from(int cmS, int cmT) {
    if ((cmS & 0x2) && (cmT & 0x2)) {
        return PVR_UVCLAMP_UV;
    }
    if (cmS & 0x2) {
        return PVR_UVCLAMP_U;
    }
    if (cmT & 0x2) {
        return PVR_UVCLAMP_V;
    }
    return PVR_UVCLAMP_NONE;
}

static int uv_flip_from(int cmS, int cmT) {
    int flip = PVR_UVFLIP_NONE;
    if (cmS & 0x1) {
        flip |= PVR_UVFLIP_U;
    }
    if (cmT & 0x1) {
        flip |= PVR_UVFLIP_V;
    }
    return flip;
}

// Smallest power of two >= v, clamped to the PVR's [8, 1024] texture range.
static int next_pot(int v) {
    int p = 8;
    while (p < v) {
        p <<= 1;
    }
    return (p > 1024) ? 1024 : p;
}

// The PVR only samples power-of-two textures, and its twiddle loader assumes POT
// dims. N64 textures are often non-POT, so — the way OoT's DC port does it — pad
// the image up into a POT buffer (edge-replicated so bilinear filtering doesn't
// bleed the border), store it NON-twiddled (a plain linear copy, no twiddle
// arithmetic to go out of bounds), and fold the padding ratio into the UVs at
// draw time via u_scale/v_scale.
unsigned int gfx_create_texture(const void *rgba, int width, int height, int cmS, int cmT) {
    const unsigned char *src = (const unsigned char *) rgba;
    unsigned short *tmp;
    pvr_ptr_t vram;
    int slot;
    int pw, ph;
    int x, y;
    int opaque = 1;

    if (!sPvrReady || rgba == NULL || width <= 0 || height <= 0) {
        return 0;
    }

    for (slot = 0; slot < MAX_TEXTURES; slot++) {
        if (sTextures[slot].data == NULL) {
            break;
        }
    }
    if (slot == MAX_TEXTURES) {
        printf("gfx: TEXTURE SLOTS FULL (%d)\n", MAX_TEXTURES);
        return 0;
    }

    pw = next_pot(width);
    ph = next_pot(height);

    tmp = (unsigned short *) malloc((size_t) pw * ph * 2);
    if (tmp == NULL) {
        return 0;
    }
    // RGBA8888 -> ARGB4444 into the padded buffer. Samples outside the original
    // clamp to its edge (replicate), which keeps the padding from darkening the
    // border under bilinear filtering.
    for (y = 0; y < ph; y++) {
        int sy = (y < height) ? y : height - 1;
        for (x = 0; x < pw; x++) {
            int sx = (x < width) ? x : width - 1;
            const unsigned char *p = &src[(sy * width + sx) * 4];
            unsigned int r = p[0] >> 4;
            unsigned int g = p[1] >> 4;
            unsigned int b = p[2] >> 4;
            unsigned int a = p[3] >> 4;
            if (a != 0xF && x < width && y < height) {
                opaque = 0;
            }
            tmp[y * pw + x] = (unsigned short) ((a << 12) | (r << 8) | (g << 4) | b);
        }
    }

    vram = pvr_mem_malloc((size_t) pw * ph * 2);
    if (vram == NULL) {
        printf("gfx: VRAM ALLOC FAILED %dx%d (padded %dx%d, %d bytes), %d free, %d frees held\n", width, height,
               pw, ph, pw * ph * 2, (int) pvr_mem_available(), sPendingFreeCount);
        free(tmp);
        return 0;
    }
    pvr_txr_load(tmp, vram, (uint32) (pw * ph * 2)); // plain copy, non-twiddled
    free(tmp);

    sTextures[slot].data = vram;
    sTextures[slot].w = width;
    sTextures[slot].h = height;
    sTextures[slot].padded_w = pw;
    sTextures[slot].padded_h = ph;
    sTextures[slot].u_scale = (float) width / (float) pw;
    sTextures[slot].v_scale = (float) height / (float) ph;
    sTextures[slot].cmS = cmS;
    sTextures[slot].cmT = cmT;
    sTextures[slot].opaque = opaque;
    return (unsigned int) (slot + 1);
}

void gfx_delete_texture(unsigned int handle) {
    int slot;

    if (handle == 0 || handle > MAX_TEXTURES) {
        return;
    }
    slot = (int) handle - 1;
    if (sTextures[slot].data != NULL) {
        if (sInScene && sPendingFreeCount < PENDING_FREE_MAX) {
            sPendingFree[sPendingFreeCount++] = sTextures[slot].data;
        } else {
            // Outside a scene, or more evictions in one frame than the queue holds.
            // Freeing now can only corrupt a recording that already referenced it,
            // and leaking the block instead would be worse.
            pvr_mem_free(sTextures[slot].data);
        }
        sTextures[slot].data = NULL;
    }
    if (sBoundTex == (int) handle) {
        sBoundTex = 0;
        sHdrDirty = 1;
    }
}

void gfx_bind_texture(unsigned int handle) {
    if (handle > MAX_TEXTURES) {
        handle = 0;
    }
    if ((int) handle != sBoundTex) {
        sBoundTex = (int) handle;
        sHdrDirty = 1;
    }
}

void gfx_set_texture_filter(int point) {
    if (point != sFilterPoint) {
        sFilterPoint = point;
        sHdrDirty = 1;
    }
}

// See sTexEnvBlend above. Store the constant and switch the draw path to source
// the vertex colour from it.
void gfx_set_texenv_blend(const unsigned char color[4]) {
    sTexEnvBlend = 1;
    if (color != NULL) {
        sBlendColor[0] = color[0];
        sBlendColor[1] = color[1];
        sBlendColor[2] = color[2];
        sBlendColor[3] = color[3];
    }
}

void gfx_set_texenv_modulate(void) {
    sTexEnvBlend = 0;
}

// ---------------------------------------------------------------------------
// Render-mode state
// ---------------------------------------------------------------------------

// No fog in the first pass — accepted as a cosmetic gap.
void gfx_set_fog(int enable, const unsigned char color[4]) {
    (void) enable;
    (void) color;
}

void gfx_set_depth_test(int enable) {
    if (enable != sDepthTest) {
        sDepthTest = enable;
        sHdrDirty = 1;
    }
}

void gfx_set_depth_write(int enable) {
    if (enable != sDepthWrite) {
        sDepthWrite = enable;
        sHdrDirty = 1;
    }
}

void gfx_set_depth_offset(int enable) {
    sDepthOffset = enable; // applied per-vertex, no header change
}

void gfx_set_alpha_test(float ref) {
    // Only whether a test is wanted matters, not the threshold: every material that
    // asks for one asks for the same 0.5. It changes the header (see
    // compile_header_for), so a change has to dirty it.
    if ((ref > 0.0f) != (sAlphaRef > 0.0f)) {
        sHdrDirty = 1;
    }
    sAlphaRef = ref;
}

void gfx_set_scissor(float x0, float y0, float x1, float y1) {
    sScisEnable = 1;
    sScisX0 = x0;
    sScisY0 = y0;
    sScisX1 = x1;
    sScisY1 = y1;
    sScisDirty = 1;
    sScisDirtyPt = 1;
    sScisDirtyOp = 1;
}

void gfx_disable_scissor(void) {
    if (sScisEnable) {
        sScisEnable = 0;
        sScisDirty = 1;
        sScisDirtyPt = 1;
        sScisDirtyOp = 1;
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void gfx_frame_begin(void) {
    if (!sPvrReady) {
        return;
    }
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_dr_init(&sDrState);
    sInScene = 1;
    sHdrDirty = 1;
    sScisDirty = 1;
    sScisDirtyPt = 1;
    sScisDirtyOp = 1;
    sPtCount = 0;
    sOpCount = 0;
    sRoute = ROUTE_TR;
}

// User clip is specified in 32-pixel tiles, lower-right inclusive.
static void submit_user_clip(int x0, int y0, int x1, int y1) {
    unsigned int *clip;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
    if (y1 > DC_SCREEN_H) y1 = DC_SCREEN_H;

    clip = (unsigned int *) ta_target();
    clip[0] = PVR_CMD_USERCLIP;
    clip[1] = 0;
    clip[2] = 0;
    clip[3] = 0;
    clip[4] = (unsigned int) (x0 >> 5);       // min tile x
    clip[5] = (unsigned int) (y0 >> 5);       // min tile y
    clip[6] = (unsigned int) ((x1 - 1) >> 5); // max tile x (inclusive)
    clip[7] = (unsigned int) ((y1 - 1) >> 5); // max tile y (inclusive)
    ta_commit(clip);
}

// ---------------------------------------------------------------------------
// Exact scissor
//
// submit_user_clip() above rounds every edge out to its enclosing 32-pixel tile,
// because that is the only granularity the PVR's user clip has. The rect it
// produces is therefore never smaller than the one asked for, and can be up to 31
// pixels larger per edge — so geometry sitting within a tile of a scissor edge
// survives a clip that should have cut it. The RDP's scissor is pixel-exact, so
// the game relies on it being so: the track-select preview leaks its sky and road
// out past the picture frame, on whichever edges do not happen to land on a tile
// boundary.
//
// The fix is to clip the geometry to the true rect here, on the CPU. The tile clip
// stays as a free coarse reject; this only has to be exact in the boundary tiles.
// Nothing is done unless a scissor is actually set and at least one of its edges is
// misaligned, so the common cases (no scissor, or a full-screen one) cost a compare.
// ---------------------------------------------------------------------------

/** True when the tile-rounded clip would take in pixels the real one excludes. */
static int scissor_needs_exact_clip(void) {
    int x0, y0, x1, y1;

    if (!sScisEnable) {
        return 0;
    }
    x0 = (int) (sScisX0 * sScaleX);
    y0 = (int) (sScisY0 * sScaleY);
    x1 = (int) (sScisX1 * sScaleX);
    y1 = (int) (sScisY1 * sScaleY);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
    if (y1 > DC_SCREEN_H) y1 = DC_SCREEN_H;

    // An edge on a multiple of 32 is already exact; so is one clamped to the screen.
    return ((x0 & 31) != 0) || ((y0 & 31) != 0) || ((x1 & 31) != 0) || ((y1 & 31) != 0);
}

static inline float vert_invw(const GfxTriVert *v) {
    return (v->w > 1e-6f) ? (1.0f / v->w) : 1.0e6f;
}

static inline unsigned char clamp_u8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 255.0f) return 255;
    return (unsigned char) (f + 0.5f);
}

/**
 * The vertex `s` of the way along a->b, where `s` is measured in screen space.
 *
 * Position is linear in screen space, so it interpolates directly. Everything the
 * projection divided by w is not: only attr/w is linear across the screen, so each
 * attribute is interpolated in attr/w and multiplied back out. That is the same
 * correction the PVR applies between vertices — doing it here just means the new
 * vertex carries the value the original triangle really had at that point.
 */
static void clip_lerp(GfxTriVert *out, const GfxTriVert *a, const GfxTriVert *b, float s) {
    float ia = vert_invw(a);
    float ib = vert_invw(b);
    float iw = ia + s * (ib - ia);
    float w = (iw > 1e-9f) ? (1.0f / iw) : a->w;

#define PERSP(fa, fb) ((((fa) * ia) + s * (((fb) * ib) - ((fa) * ia))) * w)
    out->x = a->x + s * (b->x - a->x);
    out->y = a->y + s * (b->y - a->y);
    out->z = a->z + s * (b->z - a->z); // unused by this backend; kept consistent
    out->w = w;
    out->u = PERSP(a->u, b->u);
    out->v = PERSP(a->v, b->v);
    out->fog = PERSP(a->fog, b->fog);
    out->r = clamp_u8(PERSP((float) a->r, (float) b->r));
    out->g = clamp_u8(PERSP((float) a->g, (float) b->g));
    out->b = clamp_u8(PERSP((float) a->b, (float) b->b));
    out->a = clamp_u8(PERSP((float) a->a, (float) b->a));
#undef PERSP
}

// A triangle against four half-planes gains at most one vertex per plane.
#define CLIP_MAX_VERTS 8

/**
 * Sutherland-Hodgman against one axis-aligned half-plane.
 * `axis` picks x (0) or y (1); `keepGreater` selects coord >= bound over <= bound.
 * Returns the new vertex count; `out` needs room for n + 1.
 */
static int clip_poly_plane(const GfxTriVert *in, int n, GfxTriVert *out, int axis, float bound,
                           int keepGreater) {
    int m = 0;
    int i;

    for (i = 0; i < n; i++) {
        const GfxTriVert *a = &in[i];
        const GfxTriVert *b = &in[(i + 1) % n];
        float ca = axis ? a->y : a->x;
        float cb = axis ? b->y : b->x;
        int ina = keepGreater ? (ca >= bound) : (ca <= bound);
        int inb = keepGreater ? (cb >= bound) : (cb <= bound);

        if (ina) {
            out[m++] = *a;
        }
        if (ina != inb) {
            float d = cb - ca;
            float s = (d != 0.0f) ? ((bound - ca) / d) : 0.0f;

            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            clip_lerp(&out[m++], a, b, s);
        }
    }
    return m;
}

/**
 * Clips one triangle to the scissor rect. Returns the resulting convex polygon's
 * vertex count in `poly` (0 when nothing survives), to be fanned into triangles.
 */
static int clip_tri_to_scissor(const GfxTriVert *tri, GfxTriVert *poly) {
    GfxTriVert a[CLIP_MAX_VERTS], b[CLIP_MAX_VERTS];
    float minX, maxX, minY, maxY;
    int n;

    minX = maxX = tri[0].x;
    minY = maxY = tri[0].y;
    for (n = 1; n < 3; n++) {
        if (tri[n].x < minX) minX = tri[n].x;
        if (tri[n].x > maxX) maxX = tri[n].x;
        if (tri[n].y < minY) minY = tri[n].y;
        if (tri[n].y > maxY) maxY = tri[n].y;
    }
    // Wholly outside: gone. Wholly inside: untouched, which is the common case and
    // keeps the clipper off the fast path for everything but the boundary tiles.
    if (maxX < sScisX0 || minX > sScisX1 || maxY < sScisY0 || minY > sScisY1) {
        return 0;
    }
    if (minX >= sScisX0 && maxX <= sScisX1 && minY >= sScisY0 && maxY <= sScisY1) {
        poly[0] = tri[0];
        poly[1] = tri[1];
        poly[2] = tri[2];
        return 3;
    }

    a[0] = tri[0];
    a[1] = tri[1];
    a[2] = tri[2];
    n = clip_poly_plane(a, 3, b, 0, sScisX0, 1);
    if (n < 3) return 0;
    n = clip_poly_plane(b, n, a, 0, sScisX1, 0);
    if (n < 3) return 0;
    n = clip_poly_plane(a, n, b, 1, sScisY0, 1);
    if (n < 3) return 0;
    n = clip_poly_plane(b, n, poly, 1, sScisY1, 0);
    if (n < 3) return 0;
    return n;
}

static void compile_header_for(int list, pvr_poly_hdr_t *out) {
    pvr_poly_cxt_t cxt;

    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        DcTexture *t = &sTextures[sBoundTex - 1];
        // Padded (power-of-two) dims, and NON-twiddled to match the plain copy in
        // gfx_create_texture.
        pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                         t->padded_w, t->padded_h, t->data,
                         sFilterPoint ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = uv_clamp_from(t->cmS, t->cmT);
        cxt.txr.uv_flip = uv_flip_from(t->cmS, t->cmT);
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }

    cxt.gen.culling = PVR_CULLING_NONE; // the game already back-face culls in SW
    cxt.gen.clip_mode = sScisEnable ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
    // Offset colour (specular) always on: it carries the combiner's additive term
    // in the texenv-blend path (oargb, added post-modulate). Non-blend draws set
    // oargb = 0, so the add is a free no-op there.
    cxt.gen.specular = PVR_SPECULAR_ENABLE;
    cxt.depth.comparison = sDepthTest ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;

    // An alpha-tested batch in the TR list cannot discard its transparent texels —
    // only the PT list can — so it must not write depth either. Otherwise those
    // texels blend away to nothing but still stamp the W-buffer, and everything
    // drawn later and behind them is depth-rejected: a sprite-shaped hole showing
    // whatever was in the framebuffer first. The RDP gets to have both because its
    // alpha compare runs before the depth stage; here it is one or the other.
    //
    // The cost is real: these sprites no longer occlude anything drawn after them,
    // so something passing behind a tree can show through it. That is the trade for
    // not having the hole, and it is only settled by classifying the lists.
    if (!USE_PT_LIST && sAlphaRef > 0.0f) {
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    } else {
        cxt.depth.write = sDepthWrite ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    }
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;

    pvr_poly_compile(out, &cxt);
}

static void compile_header(void) {
    compile_header_for(PVR_LIST_TR_POLY, &sHdr);
}

static inline unsigned int pack_argb(unsigned char a, unsigned char r, unsigned char g, unsigned char b) {
    return ((unsigned int) a << 24) | ((unsigned int) r << 16) | ((unsigned int) g << 8) | (unsigned int) b;
}

/** One PVR vertex from one GfxTriVert. `eol` ends the 3-vertex strip. */
static void emit_vert(const GfxTriVert *s, int eol, float uScale, float vScale) {
    pvr_vertex_t *v = (pvr_vertex_t *) ta_target();
    float w = s->w;
    float invw = (w > 1e-6f) ? (1.0f / w) : 1.0e6f;

    if (sDepthOffset) {
        invw *= 1.003f; // nudge decals toward the viewer
    }

    v->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = s->x * sScaleX;
    v->y = s->y * sScaleY;
    v->z = invw;
    v->u = s->u * uScale;
    v->v = s->v * vScale;
    if (sTexEnvBlend) {
        // result = unlit + texel*(lit - unlit): argb = lit - unlit (the
        // modulated part), oargb = unlit (the additive offset). unlit is
        // the per-vertex colour; lit is sBlendColor. Clamp the difference
        // at 0 — the rare lit<unlit channel just loses its texel weighting.
        int dr = (int) sBlendColor[0] - (int) s->r;
        int dg = (int) sBlendColor[1] - (int) s->g;
        int db = (int) sBlendColor[2] - (int) s->b;

        if (dr < 0) dr = 0;
        if (dg < 0) dg = 0;
        if (db < 0) db = 0;
        v->argb = pack_argb(sBlendColor[3], (unsigned char) dr, (unsigned char) dg, (unsigned char) db);
        v->oargb = ((unsigned int) s->r << 16) | ((unsigned int) s->g << 8) | (unsigned int) s->b;
    } else {
        v->argb = pack_argb(s->a, s->r, s->g, s->b);
        v->oargb = 0;
    }
    ta_commit(v);
}

void gfx_draw_tris(const GfxTriVert *verts, int count) {
    pvr_poly_hdr_t ptHdr __attribute__((aligned(32)));
    GfxTriVert poly[CLIP_MAX_VERTS];
    const pvr_poly_hdr_t *hdrSrc;
    pvr_poly_hdr_t *hdrDst;
    float uScale = 1.0f, vScale = 1.0f;
    int tris = count / 3;
    int outTris = tris;
    int exact;
    int i, j;

    if (!sInScene || count < 3) {
        return;
    }

    // With a misaligned scissor the tile clip is not enough, so the geometry is cut
    // to the true rect at emit time below.
    exact = scissor_needs_exact_clip();

    // Pick the list. Alpha-tested batches go to PT. Backdrop — anything that cannot
    // be depth-rejected and is drawn before the first alpha-tested batch — goes to
    // OP, because the PVR renders OP, then PT, then TR, and in TR it would land on
    // top of the sprites. Neither list can be opened until TR closes at frame end,
    // so both are recorded and replayed.
    //
    // The backdrop is recognised by what it is rather than by a front-end tag:
    // depth test off (so it paints unconditionally), opaque in both the vertex and
    // the texture (a fade overlay is the same shape of draw but translucent, and must
    // stay in TR or it would render first and be invisible), and drawn before any
    // sprite — which is what keeps the HUD, also depth-less, in TR where it belongs.
    //
    // The alpha to test is the one the batch will actually emit, which in the
    // texenv-blend path comes from sBlendColor rather than the vertex: the results
    // screen's tiled backdrop is 320 blend-path quads with a black vertex colour and
    // a fully opaque texture, and excluding the blend path outright left them in TR
    // to paint over the sky and the trees.
    {
        int emitAlpha = sTexEnvBlend ? sBlendColor[3] : verts[0].a;

        sRoute = ROUTE_TR;
        if (USE_PT_LIST && sAlphaRef > 0.0f) {
            sRoute = ROUTE_PT;
        } else if (USE_PT_LIST && sPtCount == 0 && !sDepthTest && emitAlpha == 0xFF &&
                   (sBoundTex == 0 || sTextures[sBoundTex - 1].opaque)) {
            sRoute = ROUTE_OP;
        }
    }

    // Only the recorded lists reserve space up front, and clipping changes how many
    // triangles there will be — so when it is active, count them for real first.
    // TR streams straight out and needs no count, which keeps the second pass off
    // the path that matters: a split-screen viewport is a misaligned rect covering
    // the whole scene. Fully-inside triangles short-circuit in clip_tri_to_scissor,
    // so even this pass is a handful of compares each.
    if (exact && sRoute != ROUTE_TR) {
        outTris = 0;
        for (i = 0; i + 3 <= count; i += 3) {
            int n = clip_tri_to_scissor(&verts[i], poly);

            if (n >= 3) {
                outTris += n - 2;
            }
        }
    }

    if (sRoute == ROUTE_PT) {
        int need = 1 + (outTris * 3) + (sScisDirtyPt ? 1 : 0);

        if (sPtCount + need > PT_MAX_WORDS) {
            if (!sPtOverflow) {
                sPtOverflow = 1;
                printf("gfx: PT record buffer full (%d words) — dropping batches\n", PT_MAX_WORDS);
            }
            sRoute = ROUTE_TR;
            return;
        }
    } else if (sRoute == ROUTE_OP) {
        int need = 1 + (outTris * 3) + (sScisDirtyOp ? 1 : 0);

        if (sOpCount + need > OP_MAX_WORDS) {
            // Falls back to TR rather than dropping the draw: a backdrop in the
            // wrong list is a sprite artefact, a missing one is a hole.
            if (!sOpOverflow) {
                sOpOverflow = 1;
                printf("gfx: OP record buffer full (%d words) — backdrop falling back to TR\n", OP_MAX_WORDS);
            }
            sRoute = ROUTE_TR;
        }
    }

    // NPOT textures are stored padded; the game's 0..1 UVs address the original,
    // so scale them into the padded texture's used region.
    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        uScale = sTextures[sBoundTex - 1].u_scale;
        vScale = sTextures[sBoundTex - 1].v_scale;
    }

    if ((sRoute == ROUTE_PT) ? sScisDirtyPt : ((sRoute == ROUTE_OP) ? sScisDirtyOp : sScisDirty)) {
        if (sScisEnable) {
            // The scissor arrives in N64 pixels; the framebuffer is scaled up, so
            // the clip rect has to be scaled the same way the vertices are — else
            // a full-screen 320x240 scissor confines everything to the top-left
            // quarter of the 640x480 output.
            submit_user_clip((int) (sScisX0 * sScaleX), (int) (sScisY0 * sScaleY),
                             (int) (sScisX1 * sScaleX), (int) (sScisY1 * sScaleY));
        } else {
            submit_user_clip(0, 0, DC_SCREEN_W, DC_SCREEN_H);
        }
        if (sRoute == ROUTE_PT) {
            sScisDirtyPt = 0;
        } else if (sRoute == ROUTE_OP) {
            sScisDirtyOp = 0;
        } else {
            sScisDirty = 0;
        }
    }

    // The TR header is cached across batches; the PT one is compiled per batch,
    // which costs nothing at ~24 batches a frame and keeps the TR cache honest —
    // sHdrDirty still means "TR's copy is stale" and only the TR path clears it.
    if (sRoute == ROUTE_PT) {
        compile_header_for(PVR_LIST_PT_POLY, &ptHdr);
        hdrSrc = &ptHdr;
    } else if (sRoute == ROUTE_OP) {
        compile_header_for(PVR_LIST_OP_POLY, &ptHdr);
        hdrSrc = &ptHdr;
    } else {
        if (sHdrDirty) {
            compile_header();
            sHdrDirty = 0;
        }
        hdrSrc = &sHdr;
    }
    hdrDst = (pvr_poly_hdr_t *) ta_target();
    *hdrDst = *hdrSrc;
    ta_commit(hdrDst);

    // GL_TRIANGLES -> one 3-vertex PVR strip per triangle (3rd vertex EOL).
    for (i = 0; i + 3 <= count; i += 3) {
        if (!exact) {
            for (j = 0; j < 3; j++) {
                emit_vert(&verts[i + j], j == 2, uScale, vScale);
            }
        } else {
            // The clip turns a triangle into a convex polygon; fan it back out.
            int n = clip_tri_to_scissor(&verts[i], poly);

            for (j = 1; j + 1 < n; j++) {
                emit_vert(&poly[0], 0, uScale, vScale);
                emit_vert(&poly[j], 0, uScale, vScale);
                emit_vert(&poly[j + 1], 1, uScale, vScale);
            }
        }
    }

    sRoute = ROUTE_TR;
}

/** Open a list, push a recording into it verbatim, close it. */
static void replay_list(int list, unsigned char (*buf)[32], int count) {
    int i;

    if (count <= 0) {
        return;
    }
    pvr_list_begin(list);
    pvr_dr_init(&sDrState);
    for (i = 0; i < count; i++) {
        unsigned char *d = (unsigned char *) pvr_dr_target(sDrState);

        memcpy(d, buf[i], 32);
        pvr_dr_commit(d);
    }
    pvr_list_finish();
}

void gfx_frame_end(void) {
    if (!sInScene) {
        return;
    }
    pvr_list_finish(); // TR

    // Neither OP nor PT was opened this frame, so opening them now is legal — the
    // rule is that a list cannot be opened *again*, not that lists must go in order.
    // The PVR renders OP, then PT, then TR regardless of submission order.
    replay_list(PVR_LIST_OP_POLY, sOpBuf, sOpCount);
    replay_list(PVR_LIST_PT_POLY, sPtBuf, sPtCount);

    pvr_scene_finish();
    sInScene = 0;

    // Safe now: every recording that could name these has been submitted.
    while (sPendingFreeCount > 0) {
        pvr_mem_free(sPendingFree[--sPendingFreeCount]);
    }
}
