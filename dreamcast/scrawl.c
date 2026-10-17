// "n't" scratched into the title screen's beach, after the Nintendo logo, so
// it reads "Nintendon't". The game's one bump-mapped surface.
//
// dreamcast/scrawl/make_scrawl.py finds where the logo lies on the sand,
// draws finger strokes at its letter size and writes their normal map and a
// grid of vertices on the beach (build/dc/scrawl_grid.h); pvrtex makes the
// normal map a PVR bump texture (build/dc/scrawl.dt).
//
// The level draw leaves a marker after the logo's batch (pc_scrawl_after_batch)
// and the display-list walk draws the grid there (pc_scrawl_draw), through the
// same matrix as the level, into the translucent list. The PVR lights each
// texel from its normal as I = K1 + K2 sin S + K3 cos S cos(R - Q), for a
// texel elevation S and rotation R and a light at rotation Q, clamped to
// [0, 1]. Two passes leave flat sand as it is:
//   shade:     dest * I,       K1 = 1, K2 = 0: walls facing away get darker;
//   highlight: dest * (1 + I), K1 = 0, K2 = 0: walls facing the light lighter.
#include <dc/pvr.h>
#include <dc/fmath.h>
#include <enDjinn/enj_texture.h>

#include "types.h"
#include "gfx.h"
#include "scene_gate.h"
#include "scrawl.h"
#include "../build/dc/scrawl_grid.h"

#include <ultra64.h>

alignas(32) static const unsigned char sScrawlTexture[] = {
#embed "../build/dc/scrawl.dt"
};

extern int pc_project_world(const float world[3], float out[3]);

#define SCRAWL_LIGHT_ROTATION (F_PI * 1.75f)   // from the upper right of the texture, as the sun
#define SCRAWL_SHADE 0.4f                      // how dark a wall facing away gets
#define SCRAWL_HIGHLIGHT 0.45f                 // how light one facing the light gets
#define SCRAWL_DEPTH_NUDGE 1.01f               // 1/w raised a little: in front of the sand

static pvr_poly_hdr_t sScrawlHeader[2] __attribute__((aligned(32)));
static uint32_t sScrawlLight[2];

/** The offset colour that carries a bump light: K1, K2, K3 and Q, a byte each. */
static uint32_t bump_light(float k1, float k2, float k3, float q) {
    return ((uint32_t) (k1 * 255.0f) << 24) | ((uint32_t) (k2 * 255.0f) << 16) | ((uint32_t) (k3 * 255.0f) << 8) |
           (uint32_t) (q / (2.0f * F_PI) * 255.0f);
}
static int sScrawlState; // 0 not loaded, 1 ready, -1 failed

static int scrawl_load(void) {
    enj_texture_info_t info;
    pvr_poly_cxt_t cxt;

    if (sScrawlState) {
        return sScrawlState > 0;
    }
    sScrawlState = -1;
    if (!enj_texture_load_blob(sScrawlTexture, &info)) {
        return 0;
    }
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, info.pvrformat & 0x7FFFFFFFu, info.width, info.height, info.ptr,
                     PVR_FILTER_BILINEAR);
    cxt.gen.specular = PVR_SPECULAR_ENABLE; // the offset colour carries the light
    cxt.gen.clip_mode = PVR_USERCLIP_DISABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.txr.env = PVR_TXRENV_DECAL;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    // Mipmapped: the beach is seen at a grazing angle.
    cxt.txr.mipmap = info.flags.mipmapped ? PVR_MIPMAP_ENABLE : PVR_MIPMAP_DISABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_DESTCOLOR; // dest * I
    cxt.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&sScrawlHeader[0], &cxt);
    cxt.blend.dst = PVR_BLEND_ONE;       // dest * I + dest
    pvr_poly_compile(&sScrawlHeader[1], &cxt);
    sScrawlLight[0] = bump_light(1.0f, 0.0f, SCRAWL_SHADE, SCRAWL_LIGHT_ROTATION);
    sScrawlLight[1] = bump_light(0.0f, 0.0f, SCRAWL_HIGHLIGHT, SCRAWL_LIGHT_ROTATION);
    sScrawlState = 1;
    return 1;
}

void pc_scrawl_after_batch(int segment, int batch, void *dl) {
    Gfx **gfx = dl;

    if (segment == SCRAWL_SEGMENT && batch == SCRAWL_BATCH && pc_scene_level() == SCRAWL_LEVEL) {
        gDPNoOpTag((*gfx)++, PC_SCRAWL_TAG);
    }
}

void pc_scrawl_draw(void) {
    float screen[SCRAWL_GRID_U * SCRAWL_GRID_V][3];
    pvr_vertex_t strip[2 * SCRAWL_GRID_U];
    int i, j;

    if (!scrawl_load()) {
        return;
    }
    // All or nothing: a grid point behind the camera would need clipping.
    for (i = 0; i < SCRAWL_GRID_U * SCRAWL_GRID_V; i++) {
        if (!pc_project_world(sScrawlGrid[i], screen[i])) {
            return;
        }
    }
    // One strip per row of cells, alternating the row's top and bottom edge;
    // the whole grid once per pass.
    for (int pass = 0; pass < 2; pass++) {
        for (j = 0; j + 1 < SCRAWL_GRID_V; j++) {
            for (i = 0; i < SCRAWL_GRID_U; i++) {
                for (int k = 0; k < 2; k++) {
                    int at = (j + k) * SCRAWL_GRID_U + i;
                    pvr_vertex_t *v = &strip[2 * i + k];

                    v->flags = PVR_CMD_VERTEX;
                    v->x = screen[at][0];
                    v->y = screen[at][1];
                    v->z = SCRAWL_DEPTH_NUDGE / screen[at][2];
                    v->u = sScrawlGrid[at][3];
                    v->v = sScrawlGrid[at][4];
                    // DECAL mixes texel and vertex colour by the texel's alpha,
                    // which is where the bump intensity lands; black makes the
                    // colour the intensity itself, as in KOS's bumpmap example.
                    v->argb = 0xFF000000u;
                    v->oargb = sScrawlLight[pass];
                }
            }
            strip[2 * SCRAWL_GRID_U - 1].flags = PVR_CMD_VERTEX_EOL;
            if (!gfx_tr_native(j == 0 ? &sScrawlHeader[pass] : NULL, strip, 2 * SCRAWL_GRID_U)) {
                return;
            }
        }
    }
}
