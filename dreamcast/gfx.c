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
//   * ONE list: PVR_LIST_TR_POLY, with hardware autosort DISABLED. That makes the
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
// Known first-pass gaps, all cosmetic: no punch-through list, so alpha-tested
// texels blend instead of being clipped (can leave faint depth halos); no fog;
// the menu-highlight texenv "blend toward constant" is approximated by modulate.

#include "gfx.h"

#include <kos.h>
#include <dc/pvr.h>
#include <stdlib.h>

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

// Scissor, in N64 pixels (top-left origin). Full screen by default.
static int sScisEnable;
static float sScisX0, sScisY0, sScisX1, sScisY1;

// Recompute-on-demand flags.
static int sHdrDirty = 1;
static int sScisDirty = 1;

static pvr_poly_hdr_t sHdr __attribute__((aligned(32)));
static pvr_dr_state_t sDrState;

// ---------------------------------------------------------------------------
// Init / teardown
// ---------------------------------------------------------------------------

void gfx_window_init(int width, int height, int scale) {
    pvr_init_params_t params = {
        // Only the TR bin is used this first pass; everything is submitted there.
        { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },
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

    if (!sPvrReady || rgba == NULL || width <= 0 || height <= 0) {
        return 0;
    }

    for (slot = 0; slot < MAX_TEXTURES; slot++) {
        if (sTextures[slot].data == NULL) {
            break;
        }
    }
    if (slot == MAX_TEXTURES) {
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
            tmp[y * pw + x] = (unsigned short) ((a << 12) | (r << 8) | (g << 4) | b);
        }
    }

    vram = pvr_mem_malloc((size_t) pw * ph * 2);
    if (vram == NULL) {
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
    return (unsigned int) (slot + 1);
}

void gfx_delete_texture(unsigned int handle) {
    int slot;

    if (handle == 0 || handle > MAX_TEXTURES) {
        return;
    }
    slot = (int) handle - 1;
    if (sTextures[slot].data != NULL) {
        pvr_mem_free(sTextures[slot].data);
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
    // No punch-through list in this first pass: in the TR list a texel's own
    // alpha already blends it out, so the threshold is a no-op here.
    (void) ref;
}

void gfx_set_scissor(float x0, float y0, float x1, float y1) {
    sScisEnable = 1;
    sScisX0 = x0;
    sScisY0 = y0;
    sScisX1 = x1;
    sScisY1 = y1;
    sScisDirty = 1;
}

void gfx_disable_scissor(void) {
    if (sScisEnable) {
        sScisEnable = 0;
        sScisDirty = 1;
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
}

// User clip is specified in 32-pixel tiles, lower-right inclusive.
static void submit_user_clip(int x0, int y0, int x1, int y1) {
    unsigned int *clip;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DC_SCREEN_W) x1 = DC_SCREEN_W;
    if (y1 > DC_SCREEN_H) y1 = DC_SCREEN_H;

    clip = (unsigned int *) pvr_dr_target(sDrState);
    clip[0] = PVR_CMD_USERCLIP;
    clip[1] = 0;
    clip[2] = 0;
    clip[3] = 0;
    clip[4] = (unsigned int) (x0 >> 5);       // min tile x
    clip[5] = (unsigned int) (y0 >> 5);       // min tile y
    clip[6] = (unsigned int) ((x1 - 1) >> 5); // max tile x (inclusive)
    clip[7] = (unsigned int) ((y1 - 1) >> 5); // max tile y (inclusive)
    pvr_dr_commit(clip);
}

static void compile_header(void) {
    pvr_poly_cxt_t cxt;

    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        DcTexture *t = &sTextures[sBoundTex - 1];
        // Padded (power-of-two) dims, and NON-twiddled to match the plain copy in
        // gfx_create_texture.
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                         t->padded_w, t->padded_h, t->data,
                         sFilterPoint ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = uv_clamp_from(t->cmS, t->cmT);
        cxt.txr.uv_flip = uv_flip_from(t->cmS, t->cmT);
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    } else {
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    }

    cxt.gen.culling = PVR_CULLING_NONE; // the game already back-face culls in SW
    cxt.gen.clip_mode = sScisEnable ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
    // Offset colour (specular) always on: it carries the combiner's additive term
    // in the texenv-blend path (oargb, added post-modulate). Non-blend draws set
    // oargb = 0, so the add is a free no-op there.
    cxt.gen.specular = PVR_SPECULAR_ENABLE;
    cxt.depth.comparison = sDepthTest ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = sDepthWrite ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;

    pvr_poly_compile(&sHdr, &cxt);
}

static inline unsigned int pack_argb(unsigned char a, unsigned char r, unsigned char g, unsigned char b) {
    return ((unsigned int) a << 24) | ((unsigned int) r << 16) | ((unsigned int) g << 8) | (unsigned int) b;
}

void gfx_draw_tris(const GfxTriVert *verts, int count) {
    pvr_poly_hdr_t *hdrDst;
    float uScale = 1.0f, vScale = 1.0f;
    int i, j;

    if (!sInScene || count < 3) {
        return;
    }

    // NPOT textures are stored padded; the game's 0..1 UVs address the original,
    // so scale them into the padded texture's used region.
    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        uScale = sTextures[sBoundTex - 1].u_scale;
        vScale = sTextures[sBoundTex - 1].v_scale;
    }

    if (sScisDirty) {
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
        sScisDirty = 0;
    }

    if (sHdrDirty) {
        compile_header();
        sHdrDirty = 0;
    }
    hdrDst = (pvr_poly_hdr_t *) pvr_dr_target(sDrState);
    *hdrDst = sHdr;
    pvr_dr_commit(hdrDst);

    // GL_TRIANGLES -> one 3-vertex PVR strip per triangle (3rd vertex EOL).
    for (i = 0; i + 3 <= count; i += 3) {
        for (j = 0; j < 3; j++) {
            const GfxTriVert *s = &verts[i + j];
            pvr_vertex_t *v = (pvr_vertex_t *) pvr_dr_target(sDrState);
            float w = s->w;
            float invw = (w > 1e-6f) ? (1.0f / w) : 1.0e6f;

            if (sDepthOffset) {
                invw *= 1.003f; // nudge decals toward the viewer
            }

            v->flags = (j == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
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
            pvr_dr_commit(v);
        }
    }
}

void gfx_frame_end(void) {
    if (!sInScene) {
        return;
    }
    pvr_list_finish();
    pvr_scene_finish();
    sInScene = 0;
}
