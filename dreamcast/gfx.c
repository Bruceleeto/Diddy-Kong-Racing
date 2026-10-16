
#include "gfx.h"

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sh4zam/shz_sh4zam.h>
#include <enDjinn/enj_font.h>
#include <enDjinn/enj_qfont.h>
#if DKR_ENJ_RENDER
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>
#endif

// ---------------------------------------------------------------------------
// State mirrored from the gfx.h setters
// ---------------------------------------------------------------------------

#define DC_SCREEN_W 640
// With FSAA the PVR renders twice as wide and filters back down on output, so
// everything horizontal - vertices, clip tiles - is in a 1280-wide space.
#ifndef DKR_FSAA
#define DKR_FSAA 0
#endif
#define DC_RENDER_W (DC_SCREEN_W << DKR_FSAA)
#ifndef DKR_CUTOUT_1555
#define DKR_CUTOUT_1555 1
#endif
#ifndef DKR_TWIDDLE
#define DKR_TWIDDLE 1
#endif
// How cutout textures are stored and drawn. Set at build time; the debug view
// (DEBUG_VIEW=1) switches them while a frame is held, to compare the results.
static int sCutout1555 = DKR_CUTOUT_1555; // 5-5-5-1 instead of 4-4-4-4
static int sCutoutBleed = 1;              // invisible texels take their neighbours' colour

/** Takes effect for textures created afterwards, so the caller drops its texture cache. */
void gfx_set_cutout_mode(int use1555, int bleed) {
    sCutout1555 = use1555;
    sCutoutBleed = bleed;
}
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
    // Every texel fully opaque or fully transparent, and at least one transparent:
    // a cutout, which the punch-through list can draw without blending.
    int cutout;
    int format; // PVR_TXRFMT_RGB565, _ARGB1555 or _ARGB4444, chosen from the above
    int linear;    // stored in rows rather than twiddled; -1: format holds all layout bits
    int mipmapped; // has mipmaps (adopted textures only)
} DcTexture;

static DcTexture sTextures[MAX_TEXTURES];

static int sPvrReady;
static int sFpsFont; // enDjinn's quick font is in video memory, for the frame rate
static int sInScene;

// N64 framebuffer size the game draws in, and how we stretch it to fill the DC
// 640x480 output.
static float sScaleX = 2.0f;
static float sScaleY = 2.0f;

// Current render state.
static int sBoundTex;      // 1-based index into sTextures, 0 = untextured
static int sFilterPoint;   // 1 = nearest, 0 = bilinear
// texenv "blend toward constant" (textured 2D: font glyphs, logos, menu
// highlight). The RDP combiner is linear in the texel:
//     result = unlit + texel * (lit - unlit)
// with `unlit`/`lit` the combiner at texel 0 and 1. The interpreter gives `unlit`
// per-vertex (the vertex colour) and `lit` as this constant. The PVR computes
// texel*argb + oargb (offset colour, post-modulate), so argb = lit - unlit,
// oargb = unlit. The selection highlight lives in per-vertex `unlit`, riding in
// oargb.
static int sTexEnvBlend;
static unsigned char sBlendColor[4]; // `lit`
static int sDepthTest = 1;
static int sDepthWrite = 1;
static int sDepthOffset;   // decal bias
static float sAlphaRef;    // > 0 routes the batch to the punch-through list
static int sBlend = 1;     // the RDP render mode blends (FORCE_BL): translucent

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

// List routing. The PVR renders OP, then PT, then TR, whatever order they were
// submitted in, and only one list can be open for direct submission at a time.
//
//   OP  opaque surfaces: everything the RDP draws without blending. Streamed
//       straight to the store queues, because it is most of a frame. The PVR
//       resolves visibility here before it shades, so hidden surfaces are free.
//   PT  alpha-tested cutouts (trees, fences). Recorded, replayed at frame end.
//   TR  blended surfaces and the 2D layer drawn over the scene. Recorded in
//       display-list order and replayed last.
//
// Headers, vertices and user-clip commands are all one 32-byte TA word, so a
// batch is recorded as the word stream it would have been submitted as and
// replayed verbatim. On overflow the surplus is dropped and reported once.
#define USE_PT_LIST 1

#define PT_MAX_WORDS 4096

static unsigned char sPtBuf[PT_MAX_WORDS][32] __attribute__((aligned(32)));
static int sPtCount;
static int sPtOverflow;

#define TR_MAX_WORDS 8192

static unsigned char sTrBuf[TR_MAX_WORDS][32] __attribute__((aligned(32)));
static int sTrCount;
static int sTrOverflow;

// Display-list order matters only for draws without a depth test. Before the
// first depth-tested batch they are the backdrop (screen fill, sky); after it
// they are the overlay (HUD, text, fades).
static int sSeenDepth;
// A blended backdrop batch is replayed after the opaque scene, so it is drawn at
// this depth with a depth compare: just in front of the background plane
// (FLT_EPSILON in KOS) and behind any real surface.
#define BACKDROP_Z 1.0e-6f
static int sFarBackdrop;

// VRAM freed while a scene is open. A recorded header holds a raw texture
// pointer and isn't replayed until frame end, so freeing mid-frame lets the next
// pvr_mem_malloc hand the same block back and the recording draws whatever
// landed there. Frees are held until after the replay. Only matters for the
// recorded lists; OP is submitted as it goes.
#define PENDING_FREE_MAX 512

static pvr_ptr_t sPendingFree[PENDING_FREE_MAX];
static int sPendingFreeCount;

// Where the batch currently being emitted is going.
#define ROUTE_TR 0
#define ROUTE_PT 1
#define ROUTE_OP 2

static int sRoute;

// PVR punch-through alpha threshold: a texel with alpha below this is discarded
// before the depth stage. KOS names the register's offset (PVR_PT_ALPHA_REF) but
// never writes it; this is its full address. 0x80 is the RDP's ref = 0.5, which
// is what every alpha-tested material in the game asks for.
#define DKR_PT_ALPHA_REF_ADDR 0xA05F811C
#define PT_ALPHA_REF_VALUE 0x80

/** Where the next 32-byte TA word goes: the store queues, or one of the recordings. */
static void *ta_target(void) {
    if (sRoute == ROUTE_PT) {
        return sPtBuf[sPtCount];
    }
    if (sRoute == ROUTE_TR) {
        return sTrBuf[sTrCount];
    }
    return pvr_dr_target(sDrState);
}

static void ta_commit(void *p) {
    if (sRoute == ROUTE_PT) {
        sPtCount++;
    } else if (sRoute == ROUTE_TR) {
        sTrCount++;
    } else {
        pvr_dr_commit(p);
    }
}

// ---------------------------------------------------------------------------
// Init / teardown
// ---------------------------------------------------------------------------

static void* pvr_ta_sq_addr;

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
        DKR_FSAA,   // horizontal supersampling
        1,          // autosort DISABLED -> submission order preserved
        3           // OPB overflow count
    };

    (void) scale; // the DC always outputs 640x480; we stretch to fit

    if (sPvrReady) {
        return;
    }

    sScaleX = (float) DC_RENDER_W / (float) width;
    sScaleY = (float) DC_SCREEN_H / (float) height;

    // A 24-bit framebuffer instead of the default RGB565: no banding in skies,
    // fog and fades. The packed format costs 307 KB more video memory for each
    // of the two buffers than 16-bit does; RGB0888 would cost twice that.
#if DKR_ENJ_RENDER
    // enDjinn sets the video mode and starts the PVR (ENJ_RENDER=1); AICAflow
    // keeps the AICA, and the port reads the controllers itself.
    {
        enj_state_t *state;

        enj_state_init_defaults();
        state = enj_state_get();
        state->video.display_mode = DM_640x480;
        state->video.pixel_mode = PM_RGB888P;
        state->video.pvr_params = params;
        state->flags.soft_reset_enabled = 0;
        state->flags.no_snd_init = 1;
        state->flags.no_input_init = !DKR_ENJ_INPUT; // ENJ_INPUT=1: enDjinn reads the pads
        if (enj_state_startup() != 0) {
            return;
        }
    }
#else
    vid_set_mode(DM_640x480, PM_RGB888P);

    if (pvr_init(&params) < 0) {
        return;
    }
#endif
    printf("gfx: 24-bit framebuffer, %d bytes of video memory free\n", (int) pvr_mem_available());

    // Set after pvr_init so it cannot be clobbered by it.
    *((volatile unsigned int *) DKR_PT_ALPHA_REF_ADDR) = PT_ALPHA_REF_VALUE;

    pvr_ta_sq_addr = (void *)SQ_MASK_DEST(PVR_TA_INPUT);

    // enDjinn's quick font for the frame-rate readout, in front of everything.
    // enj_state_startup has loaded it already when enDjinn renders.
#if DKR_ENJ_RENDER
    if (enj_qfont_get_header() != NULL) {
#else
    if (enj_qfont_init() == 0) {
#endif
        enj_font_zvalue_set(1000.0f);
        sFpsFont = 1;
    }

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
static void header_cache_clear(void);

unsigned int gfx_create_texture(const void *rgba, int width, int height, int cmS, int cmT) {
    const unsigned char *src = (const unsigned char *) rgba;
    unsigned short *tmp;
    pvr_ptr_t vram;
    int slot;
    int pw, ph;
    int x, y;

    header_cache_clear();
    int opaque = 1;
    int cutout = 1;
    int format;

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
    // What the alpha channel is used for decides the format. All three are 16
    // bits a texel, so the choice costs no memory: a texture with no alpha keeps
    // 5-6-5 bits of colour, a cutout 5-5-5 plus its one bit of alpha, and only
    // one with real translucency drops to 4 bits a channel. Most of the game's
    // textures are 5-5-5-1 on the cartridge and so come through unchanged.
    //
    // DKR_CUTOUT_1555=0 keeps cutouts at 4 bits a channel, for comparing edges.
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned int a = src[(y * width + x) * 4 + 3] >> 4;

            if (a != 0xF) {
                opaque = 0;
                if (a != 0) {
                    cutout = 0;
                }
            }
        }
    }
    cutout = cutout && !opaque;
    format = opaque ? PVR_TXRFMT_RGB565 : (cutout && sCutout1555) ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_ARGB4444;

    // Into the padded buffer. Samples outside the original clamp to its edge
    // (replicate), which keeps the padding from darkening the border under
    // bilinear filtering.
    for (y = 0; y < ph; y++) {
        int sy = (y < height) ? y : height - 1;
        for (x = 0; x < pw; x++) {
            int sx = (x < width) ? x : width - 1;
            const unsigned char *p = &src[(sy * width + sx) * 4];
            unsigned int r = p[0], g = p[1], b = p[2], a = p[3];

            if (opaque) {
                tmp[y * pw + x] = (unsigned short) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            } else {
                if ((a >> 4) == 0 && sCutoutBleed) {
                    // An invisible texel still lends its colour to the visible
                    // ones beside it when the texture is filtered, and on the
                    // cartridge that colour is usually black, which draws a dark
                    // rim round every plant and sprite. Give it the average
                    // colour of its visible neighbours instead.
                    unsigned int nr = 0, ng = 0, nb = 0, n = 0;
                    int dx, dy;

                    for (dy = -1; dy <= 1; dy++) {
                        for (dx = -1; dx <= 1; dx++) {
                            int qx = sx + dx, qy = sy + dy;

                            if (qx >= 0 && qx < width && qy >= 0 && qy < height) {
                                const unsigned char *q = &src[(qy * width + qx) * 4];

                                if ((q[3] >> 4) != 0) {
                                    nr += q[0];
                                    ng += q[1];
                                    nb += q[2];
                                    n++;
                                }
                            }
                        }
                    }
                    if (n != 0) {
                        r = nr / n;
                        g = ng / n;
                        b = nb / n;
                    }
                }
                if (format == PVR_TXRFMT_ARGB1555) {
                    tmp[y * pw + x] =
                        (unsigned short) (((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                } else {
                    tmp[y * pw + x] =
                        (unsigned short) (((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                }
            }
        }
    }

    vram = pvr_mem_malloc((size_t) pw * ph * 2);
    if (vram == NULL) {
        printf("gfx: VRAM ALLOC FAILED %dx%d (padded %dx%d, %d bytes), %d free, %d frees held\n", width, height,
               pw, ph, pw * ph * 2, (int) pvr_mem_available(), sPendingFreeCount);
        free(tmp);
        return 0;
    }

#if DKR_TWIDDLE
    // Twiddled: the PVR reads texels in small squares, so the four a bilinear
    // sample needs mostly arrive in one fetch. Linear rows make the texture
    // cache miss on almost every step down a row.
    pvr_txr_load_ex(tmp, vram, (uint32_t) pw, (uint32_t) ph, PVR_TXRLOAD_16BPP);
#else
    pvr_txr_load(tmp, vram, (uint32_t) (pw * ph * 2)); // plain copy, non-twiddled
#endif

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
    sTextures[slot].cutout = cutout;
    sTextures[slot].format = format;
    sTextures[slot].linear = !DKR_TWIDDLE;
    sTextures[slot].mipmapped = 0;
    return (unsigned int) (slot + 1);
}

/**
 * Takes over a texture already in video memory, loaded from a pvrtex DcTx file
 * by enDjinn (enj_texture_load_blob). format is that file's texture control
 * bits: pixel format, twiddled or not, VQ; mipmapped says whether it has
 * mipmaps. kind is 0 opaque, 1 cutout, 2 translucent.
 */
unsigned int gfx_adopt_texture(pvr_ptr_t data, int width, int height, int pw, int ph, unsigned int format,
                               int mipmapped, int kind) {
    int slot;

    if (!sPvrReady || data == NULL) {
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
    header_cache_clear();
    sTextures[slot].data = data;
    sTextures[slot].w = width;
    sTextures[slot].h = height;
    sTextures[slot].padded_w = pw;
    sTextures[slot].padded_h = ph;
    sTextures[slot].u_scale = (float) width / (float) pw;
    sTextures[slot].v_scale = (float) height / (float) ph;
    sTextures[slot].cmS = 0;
    sTextures[slot].cmT = 0;
    sTextures[slot].opaque = kind == 0;
    sTextures[slot].cutout = kind == 1;
    sTextures[slot].format = (int) format;
    sTextures[slot].linear = -1; // the format bits carry the layout
    sTextures[slot].mipmapped = mipmapped;
    return (unsigned int) (slot + 1);
}

/** The wrap and clamp a prebuilt texture is drawn with, which the display list decides. */
void gfx_texture_set_wrap(unsigned int handle, int cmS, int cmT) {
    if (handle == 0 || handle > MAX_TEXTURES || sTextures[handle - 1].data == NULL) {
        return;
    }
    if (sTextures[handle - 1].cmS != cmS || sTextures[handle - 1].cmT != cmT) {
        sTextures[handle - 1].cmS = cmS;
        sTextures[handle - 1].cmT = cmT;
        header_cache_clear();
        sHdrDirty = 1;
    }
}

void gfx_delete_texture(unsigned int handle) {
    int slot;

    if (handle == 0 || handle > MAX_TEXTURES) {
        return;
    }
    header_cache_clear();
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

void gfx_set_blend(int enable) {
    sBlend = enable; // picks the list, no header change
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

// Asked once or twice per batch, so it is worked out when the scissor changes.
static int sScisExact;
static int scissor_compute_exact(void);

static inline int scissor_needs_exact_clip(void) {
    return sScisExact;
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
    sScisExact = scissor_compute_exact();
}

void gfx_disable_scissor(void) {
    if (sScisEnable) {
        sScisEnable = 0;
        sScisDirty = 1;
        sScisDirtyPt = 1;
        sScisDirtyOp = 1;
        sScisExact = 0;
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

/** What every frame starts from, whoever opens the scene. */
static void frame_state_reset(void) {
    sInScene = 1;
    sHdrDirty = 1;
    sScisDirty = 1;
    sScisDirtyPt = 1;
    sScisDirtyOp = 1;
    sPtCount = 0;
    sTrCount = 0;
    sSeenDepth = 0;
    sRoute = ROUTE_OP;
}

void gfx_frame_begin(void) {
    if (!sPvrReady) {
        return;
    }
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    frame_state_reset();
}

// User clip is specified in 32-pixel tiles, lower-right inclusive.
static void submit_user_clip(int x0, int y0, int x1, int y1) {
    unsigned int *clip;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DC_RENDER_W) x1 = DC_RENDER_W;
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
static int scissor_compute_exact(void) {
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
    if (x1 > DC_RENDER_W) x1 = DC_RENDER_W;
    if (y1 > DC_SCREEN_H) y1 = DC_SCREEN_H;

    // An edge on a multiple of 32 is already exact; so is one clamped to the screen.
    return ((x0 & 31) != 0) || ((y0 & 31) != 0) || ((x1 & 31) != 0) || ((y1 & 31) != 0);
}

static inline float vert_invw(const GfxTriVert *v) {
    return shz_invf_fsrra(v->w); //(v->w > 1e-6f) ? (1.0f / v->w) : 1.0e6f;
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
    float w = shz_invf_fsrra(iw); //(iw > 1e-9f) ? (1.0f / iw) : a->w;

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
            float s = (d != 0.0f) ? shz_divf((bound - ca), d) : 0.0f;

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

// Compiled headers, by the state that goes into one. A frame draws a few dozen
// materials over and over, and building a header costs more than a batch's
// worth of vertices. Creating or deleting a texture empties the cache, since a
// handle can come back with other pixels behind it.
#define HDR_CACHE 256
static struct {
    unsigned int key; // 0 = empty
    pvr_poly_hdr_t hdr;
} sHdrCache[HDR_CACHE] __attribute__((aligned(32)));

static void header_cache_clear(void) {
    int i;

    for (i = 0; i < HDR_CACHE; i++) {
        sHdrCache[i].key = 0;
    }
}

static void compile_header_uncached(int list, pvr_poly_hdr_t *out);

static void compile_header_for(int list, pvr_poly_hdr_t *out) {
    // Everything compile_header_uncached reads, and a top bit so no key is 0.
    unsigned int key = 0x80000000u | ((unsigned int) list << 20) | ((unsigned int) sBoundTex << 6) |
                       ((sFilterPoint != 0) << 5) | ((sScisEnable != 0) << 4) | ((sDepthTest != 0) << 3) |
                       ((sFarBackdrop != 0) << 2) | ((sAlphaRef > 0.0f) << 1) | (sDepthWrite != 0);
    unsigned int at = (key ^ (key >> 6) ^ (key >> 13)) & (HDR_CACHE - 1);

    if (sHdrCache[at].key != key) {
        compile_header_uncached(list, &sHdrCache[at].hdr);
        sHdrCache[at].key = key;
    }
    *out = sHdrCache[at].hdr;
}

static void compile_header_uncached(int list, pvr_poly_hdr_t *out) {
    pvr_poly_cxt_t cxt;

    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        DcTexture *t = &sTextures[sBoundTex - 1];
        // Padded (power-of-two) dims, stored as gfx_create_texture loaded them.
        pvr_poly_cxt_txr(&cxt, list,
                         t->linear < 0 ? t->format
                                       : (t->format | (t->linear ? PVR_TXRFMT_NONTWIDDLED : PVR_TXRFMT_TWIDDLED)),
                         t->padded_w, t->padded_h, t->data,
                         sFilterPoint ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = uv_clamp_from(t->cmS, t->cmT);
        cxt.txr.mipmap = t->mipmapped ? PVR_MIPMAP_ENABLE : PVR_MIPMAP_DISABLE;
        cxt.txr.uv_flip = uv_flip_from(t->cmS, t->cmT);
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }

    cxt.gen.culling = PVR_CULLING_NONE; // the game already back-face culls in SW
    cxt.gen.clip_mode = sScisEnable ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
    // Offset colour (specular) always on: carries the combiner's additive term in
    // the texenv-blend path (oargb, post-modulate). Non-blend draws set oargb = 0,
    // so the add is a no-op.
    cxt.gen.specular = PVR_SPECULAR_ENABLE;
    cxt.depth.comparison = sDepthTest ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    if (list == PVR_LIST_TR_POLY && sFarBackdrop) {
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL; // only where the scene left the background
    }

    // An alpha-tested batch in TR can't discard its transparent texels (only PT
    // can), so it must not write depth either. Otherwise those texels blend to
    // nothing but still stamp the W-buffer, and everything later and behind them
    // is depth-rejected: a sprite-shaped hole showing the old framebuffer. The RDP
    // has both because its alpha compare runs before the depth stage; here it is
    // one or the other.
    //
    // Cost: these sprites no longer occlude anything drawn after them, so
    // something passing behind a tree can show through it. Only fixed by
    // classifying the lists.
    if (!USE_PT_LIST && sAlphaRef > 0.0f) {
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    } else {
        cxt.depth.write = sDepthWrite ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    }
    if (list == PVR_LIST_OP_POLY) {
        // An RDP surface that does not blend ignores alpha entirely, in the
        // vertex and in the texel. So does this list.
        cxt.gen.alpha = PVR_ALPHA_DISABLE;
        cxt.txr.alpha = PVR_TXRALPHA_DISABLE;
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
    } else {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }

    pvr_poly_compile(out, &cxt);
}

/** The streamed list's header is cached across batches; the recorded ones compile per batch. */
static void compile_header(void) {
    compile_header_for(PVR_LIST_OP_POLY, &sHdr);
}

static inline unsigned int pack_argb(unsigned char a, unsigned char r, unsigned char g, unsigned char b) {
    return ((unsigned int) a << 24) | ((unsigned int) r << 16) | ((unsigned int) g << 8) | (unsigned int) b;
}

/** One PVR vertex from one GfxTriVert. `eol` ends the 3-vertex strip. */
static void emit_vert(const GfxTriVert *s, int eol, float uScale, float vScale) {
    pvr_vertex_t *v = (pvr_vertex_t *) ta_target();
    float w = s->w;
    float invw = shz_invf_fsrra(w); //(w > 1e-6f) ? (1.0f / w) : 1.0e6f;

    if (sDepthOffset) {
        invw *= 1.003f; // nudge decals toward the viewer
    }
    if (sFarBackdrop) {
        invw = BACKDROP_Z;
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
    GfxTriVert poly[CLIP_MAX_VERTS];
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

    // Pick the list.
    //
    // With a depth test, order does not matter and the RDP render mode decides:
    // a surface that does not blend is opaque. Without one the batch paints
    // unconditionally and display-list order is all there is. An opaque batch may
    // still go to OP while nothing has been drawn that it could cover: that is the
    // backdrop (screen fill, sky), and OP keeps its submission order. Anything
    // later is recorded to TR in order, which is what keeps the HUD on top.
    //
    // The alpha to test is the one the batch will actually emit, which in the
    // texenv-blend path comes from sBlendColor rather than the vertex: the results
    // screen's tiled backdrop is 320 blend-path quads with a black vertex colour and
    // a fully opaque texture.
    {
        int emitAlpha = sTexEnvBlend ? sBlendColor[3] : verts[0].a;

        sFarBackdrop = 0;
        if (USE_PT_LIST && sAlphaRef > 0.0f) {
            sRoute = ROUTE_PT;
        } else if (sDepthTest) {
            sRoute = sBlend ? ROUTE_TR : ROUTE_OP;
        } else if (!sSeenDepth && sTrCount == 0 && sPtCount == 0 && emitAlpha == 0xFF &&
                   (sBoundTex == 0 || sTextures[sBoundTex - 1].opaque)) {
            sRoute = ROUTE_OP;
        } else {
            sRoute = ROUTE_TR;
            sFarBackdrop = !sSeenDepth;
        }
        if (sDepthTest) {
            sSeenDepth = 1;
        }
    }

    // Only the recorded lists reserve space up front, and clipping changes the
    // triangle count, so count them first when it is active.
    // OP streams straight out and needs no count, which keeps the second pass off
    // the path that matters: a split-screen viewport is a misaligned rect covering
    // the whole scene. Fully-inside triangles short-circuit in clip_tri_to_scissor,
    // so even this pass is a handful of compares each.
    if (exact && sRoute != ROUTE_OP) {
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
            sRoute = ROUTE_OP;
            sFarBackdrop = 0;
            return;
        }
    } else if (sRoute == ROUTE_TR) {
        int need = 1 + (outTris * 3) + (sScisDirty ? 1 : 0);

        if (sTrCount + need > TR_MAX_WORDS) {
            if (!sTrOverflow) {
                sTrOverflow = 1;
                printf("gfx: TR record buffer full (%d words) — dropping batches\n", TR_MAX_WORDS);
            }
            sRoute = ROUTE_OP;
            sFarBackdrop = 0;
            return;
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
            submit_user_clip(0, 0, DC_RENDER_W, DC_SCREEN_H);
        }
        if (sRoute == ROUTE_PT) {
            sScisDirtyPt = 0;
        } else if (sRoute == ROUTE_OP) {
            sScisDirtyOp = 0;
        } else {
            sScisDirty = 0;
        }
    }

    if (sRoute == ROUTE_PT) {
        hdrDst = (pvr_poly_hdr_t*)ta_target();
        compile_header_for(PVR_LIST_PT_POLY, hdrDst);
        ta_commit(hdrDst);
    } else if (sRoute == ROUTE_TR) {
        hdrDst = (pvr_poly_hdr_t*)ta_target();
        compile_header_for(PVR_LIST_TR_POLY, hdrDst);
        ta_commit(hdrDst);
    } else {
        if (sHdrDirty) {
            compile_header();
            sHdrDirty = 0;
        }
        shz_sq_memcpy32_1(pvr_ta_sq_addr, &sHdr);
    }

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

    sRoute = ROUTE_OP;
    sFarBackdrop = 0;
}

// ---------------------------------------------------------------------------
// Blast streaming — the native path for static level geometry (see handle_blast
// in main.c). The caller transforms and writes the PVR vertices; this end owns
// list routing, scissor and header, consistent with gfx_draw_tris.
//
// An opaque batch streams to the open OP list. A blended one is written into the
// TR recording: begin() reserves room and hands back where to write, and the
// caller reports how far it got with gfx_blast_end(). State needing the PT list,
// the texenv-blend colour split, or exact scissor clipping makes begin() refuse,
// and the caller falls back to the emulated path.
// ---------------------------------------------------------------------------

/**
 * Whether the current state is one gfx_blast_begin would accept. Checked before
 * the caller transforms anything, so a doomed batch costs a compare, not a
 * wasted transform.
 */
/** Profile only: 0 no texture bound, 1 opaque, 2 cutout, 3 has partly transparent texels. */
int gfx_texture_kind(void) {
    if (sBoundTex == 0 || sTextures[sBoundTex - 1].data == NULL) {
        return 0;
    }
    return sTextures[sBoundTex - 1].opaque ? 1 : sTextures[sBoundTex - 1].cutout ? 2 : 3;
}

/** Which condition keeps the current state off the fast path (profile only). */
int gfx_blast_why(void) {
    return !sInScene ? 0 : sTexEnvBlend ? 1 : (sAlphaRef > 0.0f) ? 2 : !sDepthTest ? 3 : sDepthOffset ? 4 : 5;
}

int gfx_blast_viable(void) {
    return sInScene && !sTexEnvBlend && !(sAlphaRef > 0.0f && !USE_PT_LIST) && !scissor_needs_exact_clip();
}

static int sBlastRoute; // the list the open blast batch is recorded to, or ROUTE_OP

// Profile only: what the fast path sends to the blended list, in batches and
// reserved words, by whether it needed to blend. Classes: 0 no depth test (2D,
// backdrop), 1 looks opaque and writes depth, 2 looks opaque but does not write
// depth, 3 a cutout (full vertex alpha, texels all opaque or all clear), which
// punch-through could draw, 4 really translucent. "Looks" goes by the first
// vertex's alpha and the texture's texels.
static unsigned int sTrStats[10];

// Profile only: fast-path batches drawn without a depth test, in reserved
// words: 0 went to the opaque list, 1 came after depth-tested geometry, 2 came
// after something was already recorded, 3 was not opaque itself.
static unsigned int sNoDepthStats[4];

void gfx_nodepth_stats(unsigned int out[4]) {
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = sNoDepthStats[i];
        sNoDepthStats[i] = 0;
    }
}

void gfx_tr_stats(unsigned int out[10]) {
    int i;

    for (i = 0; i < 10; i++) {
        out[i] = sTrStats[i];
        sTrStats[i] = 0;
    }
}

int gfx_blast_begin(float *uScale, float *vScale, float *scaleX, float *scaleY, int maxWords,
                    void **record, int emitAlpha, float *zMul, float *zAdd) {
    int route;
    int farBackdrop = 0;

    if (!gfx_blast_viable()) {
        return 0;
    }

    // The list, exactly as gfx_draw_tris picks it.
    if (USE_PT_LIST && sAlphaRef > 0.0f) {
        route = ROUTE_PT;
    } else if (sDepthTest) {
        route = sBlend ? ROUTE_TR : ROUTE_OP;
    } else if (!sSeenDepth && sTrCount == 0 && sPtCount == 0 && emitAlpha == 0xFF &&
               (sBoundTex == 0 || sTextures[sBoundTex - 1].opaque)) {
        route = ROUTE_OP;
    } else {
        route = ROUTE_TR;
        farBackdrop = !sSeenDepth;
    }
    if (!sDepthTest && !(USE_PT_LIST && sAlphaRef > 0.0f)) {
        int opaque = emitAlpha == 0xFF && (sBoundTex == 0 || sTextures[sBoundTex - 1].opaque);

        sNoDepthStats[route == ROUTE_OP ? 0 : sSeenDepth ? 1 : (sTrCount || sPtCount) ? 2 : !opaque ? 3 : 0] +=
            (unsigned int) maxWords;
    }
    if (route == ROUTE_PT && sPtCount + maxWords + 2 > PT_MAX_WORDS) {
        return 0; // gfx_draw_tris reports the overflow
    }
    if (route == ROUTE_TR && sTrCount + maxWords + 2 > TR_MAX_WORDS) {
        return 0;
    }
    if (route == ROUTE_TR) {
        int opaque = emitAlpha == 0xFF && (sBoundTex == 0 || sTextures[sBoundTex - 1].opaque);
        int cutout = emitAlpha == 0xFF && sBoundTex != 0 && sTextures[sBoundTex - 1].cutout;
        int kind = !sDepthTest ? 0 : opaque ? (sDepthWrite ? 1 : 2) : cutout ? 3 : 4;

        sTrStats[kind]++;
        sTrStats[5 + kind] += (unsigned int) maxWords;
    }
    if (sDepthTest) {
        sSeenDepth = 1;
    }

    // Depth as emit_vert sets it: decals sit a touch nearer, and a backdrop
    // drawn after the scene's first depth-tested batch goes to the far plane.
    *zMul = farBackdrop ? 0.0f : (sDepthOffset ? 1.003f : 1.0f);
    *zAdd = farBackdrop ? BACKDROP_Z : 0.0f;

    *record = NULL;
    sBlastRoute = route;
    if (route != ROUTE_OP) {
        int *dirty = (route == ROUTE_PT) ? &sScisDirtyPt : &sScisDirty;
        pvr_poly_hdr_t *hdr;

        sRoute = route;
        if (*dirty) {
            if (sScisEnable) {
                submit_user_clip((int) (sScisX0 * sScaleX), (int) (sScisY0 * sScaleY),
                                 (int) (sScisX1 * sScaleX), (int) (sScisY1 * sScaleY));
            } else {
                submit_user_clip(0, 0, DC_RENDER_W, DC_SCREEN_H);
            }
            *dirty = 0;
        }
        sFarBackdrop = farBackdrop;
        hdr = (pvr_poly_hdr_t *) ta_target();
        compile_header_for((route == ROUTE_PT) ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY, hdr);
        ta_commit(hdr);
        sFarBackdrop = 0;
        *record = (route == ROUTE_PT) ? sPtBuf[sPtCount] : sTrBuf[sTrCount];
        sRoute = ROUTE_OP;
    } else {
        if (sScisDirtyOp) {
            if (sScisEnable) {
                submit_user_clip((int) (sScisX0 * sScaleX), (int) (sScisY0 * sScaleY),
                                 (int) (sScisX1 * sScaleX), (int) (sScisY1 * sScaleY));
            } else {
                submit_user_clip(0, 0, DC_RENDER_W, DC_SCREEN_H);
            }
            sScisDirtyOp = 0;
        }
        if (sHdrDirty) {
            compile_header();
            sHdrDirty = 0;
        }
        shz_sq_memcpy32_1(pvr_ta_sq_addr, &sHdr);
    }

    if (sBoundTex != 0 && sTextures[sBoundTex - 1].data != NULL) {
        *uScale = sTextures[sBoundTex - 1].u_scale;
        *vScale = sTextures[sBoundTex - 1].v_scale;
    } else {
        *uScale = 1.0f;
        *vScale = 1.0f;
    }
    // N64-pixels -> framebuffer scale, so the caller emits vertices without a
    // cross-file call per vertex.
    *scaleX = sScaleX;
    *scaleY = sScaleY;
    return 1;
}

/**
 * Appends a polygon header (when hdr is not NULL) and vertices to the
 * translucent recording, in display-list order. Vertex x/y arrive in N64
 * pixels and are scaled to the framebuffer here. 0 if there is no room.
 */
int gfx_tr_native(const pvr_poly_hdr_t *hdr, const pvr_vertex_t *verts, int count) {
    int i;

    if (!sInScene || sTrCount + count + (hdr ? 1 : 0) > TR_MAX_WORDS) {
        return 0;
    }
    if (hdr) {
        memcpy(sTrBuf[sTrCount++], hdr, sizeof(*hdr));
    }
    for (i = 0; i < count; i++) {
        pvr_vertex_t *v = (pvr_vertex_t *) sTrBuf[sTrCount++];

        *v = verts[i];
        v->x *= sScaleX;
        v->y *= sScaleY;
    }
    return 1;
}

void gfx_recorded_words(int *pt, int *tr) {
    *pt = sPtCount;
    *tr = sTrCount;
}

void gfx_blast_end(void *recordEnd) {
    if (sBlastRoute == ROUTE_PT) {
        sPtCount = (int) ((unsigned char (*)[32]) recordEnd - sPtBuf);
    } else {
        sTrCount = (int) ((unsigned char (*)[32]) recordEnd - sTrBuf);
    }
}

/** Push a recording verbatim into the list that is open. */
static void replay_words(unsigned char (*buf)[32], int count) {
    if (count > 0) {
        shz_sq_memcpy32_xmtrx(pvr_ta_sq_addr, buf, count * 32);
    }
}

/** Open a list, push a recording into it verbatim, add `extra`, close it. */
static void replay_list(int list, unsigned char (*buf)[32], int count, void (*extra)(void)) {
    if (count <= 0 && extra == NULL) {
        return;
    }
    pvr_list_begin(list);
    replay_words(buf, count);
    if (extra != NULL) {
        extra();
    }
    pvr_list_finish();
}

// ---------------------------------------------------------------------------
// FPS measurement and its on-screen readout
// ---------------------------------------------------------------------------
static float sFps = 0.0f;
static uint64_t sLastFrameUs = 0;

// Smoothed frames-per-second, measured from the wall-clock interval between
// rendered frames.
float pc_get_fps(void) {
    return sFps;
}

static void fps_tick(void) {
    uint64_t now = timer_us_gettime64();

    if (sLastFrameUs != 0) {
        uint64_t dt = now - sLastFrameUs;
        if (dt > 0) {
            float inst = 1000000.0f / (float) dt;
            // Exponential moving average so the readout is steady, not jittery.
            sFps = (sFps == 0.0f) ? inst : (sFps * 0.9f + inst * 0.1f);
        }
    }
    sLastFrameUs = now;
 
    {
        static unsigned int frames = 0;

        if ((++frames % 120u) == 0u) {
            printf("fps: %d.%d\n", (int) sFps, ((int) (sFps * 10.0f)) % 10);
        }
    }
}

/** The frame rate in the top-right corner, white over a black shadow. */
static void fps_draw(void) {
    char text[16];
    int fps10 = (int) (sFps * 10.0f + 0.5f);
    int x;

    snprintf(text, sizeof(text), "%d.%d fps", fps10 / 10, fps10 % 10);
    x = DC_SCREEN_W - 12 - enj_font_string_width(text, enj_qfont_get_header());
    enj_qfont_color_set(0, 0, 0);
    enj_qfont_write(text, x + 1, 9, PVR_LIST_PT_POLY);
    enj_qfont_color_set(255, 255, 255);
    enj_qfont_write(text, x, 8, PVR_LIST_PT_POLY);
}

/** After the scene is submitted. */
static void frame_done(void) {
    sInScene = 0;
    // Safe now: every recording that could name these has been submitted.
    while (sPendingFreeCount > 0) {
        pvr_mem_free(sPendingFree[--sPendingFreeCount]);
    }
}

void gfx_frame_end(void) {
    if (!sInScene) {
        return;
    }
    fps_tick();
    pvr_list_finish(); // OP

    // Neither PT nor TR was opened this frame, so opening them now is legal — the
    // rule is that a list cannot be opened *again*, not that lists must go in order.
    replay_list(PVR_LIST_PT_POLY, sPtBuf, sPtCount, sFpsFont ? fps_draw : NULL);
    replay_list(PVR_LIST_TR_POLY, sTrBuf, sTrCount, NULL);

    pvr_scene_finish();
    frame_done();
}

#if DKR_ENJ_RENDER
// ENJ_RENDER=1: enDjinn's render lists submit the scene. The display-list walk
// runs inside the opaque list, as between gfx_frame_begin and gfx_frame_end;
// the punch-through and translucent recordings it makes are replayed by the
// later lists.
static void (*sFrameWalk)(void *);
static void *sFrameWalkData;

static void frame_op(void *data) {
    (void) data;
    frame_state_reset();
    sFrameWalk(sFrameWalkData);
}

static void frame_tr(void *data) {
    (void) data;
    replay_words(sTrBuf, sTrCount);
}

static void frame_pt(void *data) {
    (void) data;
    replay_words(sPtBuf, sPtCount);
    if (sFpsFont) {
        fps_draw();
    }
}

void gfx_frame_queue(void (*walk)(void *), void *data) {
    if (!sPvrReady) {
        return;
    }
    sFrameWalk = walk;
    sFrameWalkData = data;
    fps_tick();
    // The recordings are only known after the walk, but enDjinn runs the lists
    // in order (opaque first), so the later lists read them once it is done.
    enj_render_list_add(PVR_LIST_OP_POLY, frame_op, NULL);
    enj_render_list_add(PVR_LIST_TR_POLY, frame_tr, NULL);
    enj_render_list_add(PVR_LIST_PT_POLY, frame_pt, NULL);
}

void gfx_frame_render(void (*walk)(void *), void *data) {
    if (!sPvrReady) {
        return;
    }
    gfx_frame_queue(walk, data);
    enj_render_scene();
    frame_done();
}

void gfx_frame_submitted(void) {
    frame_done();
}
#endif
