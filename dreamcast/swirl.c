// The Dreamcast swirl in place of the N64 logo on the boot screen.
//
// dreamcast/swirl/make_swirl.py builds the model (build/dc/swirl.model) from
// the public-domain outline of the Dreamcast logo, in the game's object model
// format, and the surface the port draws (build/dc/swirl.surface): its
// distinct vertices and triangle strips. object_model_init asks for each model
// it loads; this one is served from here instead of from assets.bin, so the
// asset image and everything laid out from it stay as they are.
//
// The port lights and draws it itself. render_3d_model calls pc_swirl_render
// in place of the game's mesh draw, which lights each distinct vertex from the
// rendering equation: a lacquered, physically based surface (Lambert diffuse,
// Cook-Torrance specular with a GGX distribution, Smith-Schlick visibility and
// Schlick's Fresnel) under two lights that keep to the camera (a warm key from
// the upper left in front, a cool rim from behind), the sky's irradiance as a
// hemisphere, and the sky reflected, Fresnel-weighted; the sky stays put as
// the swirl turns. The result is tone mapped (ACES, fitted) to sRGB. Orange,
// and the European swirl's blue on a European console. The eye, the lights
// and the sky's up are taken into the model's own space once a frame, so each
// vertex is lit from its model-space position and normal as they are.
//
// It leaves a marker in the display list, after the swirl's matrix, and the
// walk draws it there (pc_swirl_draw): each distinct vertex through the
// matrix once, then the strips streamed into the opaque list (translucent
// while the logo fades). The boot screen's camera never brings it to the
// near plane, so it isn't clipped. The game's model is only a stand-in.
//
// Two touches: a glint, a narrow band of light that sweeps across the face
// once each time it turns square on to the camera (after it has faded in),
// and a soft halo behind it in its colour.
#include <dc/flashrom.h>
#include <dc/pvr.h>
#include <stdlib.h>
#include <string.h>
#include <sh4zam/shz_sh4zam.h>

#include "types.h"
#include "structs.h"
#include "asset_enums.h"
#include "camera.h"
#include "math_util.h"
#include "gfx.h"
#include "swirl.h"

#include <ultra64.h>

alignas(32) static const unsigned char sSwirlModel[] = {
#embed "../build/dc/swirl.model"
};
alignas(32) static const unsigned char sSwirlSurface[] = {
#embed "../build/dc/swirl.surface"
};
alignas(32) static const unsigned char sSwirlGlow[] = {
#embed "../build/dc/swirl.glow"
};

// The surface's layout (make_swirl.py).
typedef struct {
    u32 points, strips, indices;
} SwirlCounts;
typedef struct {
    float px, py, pz;
    float nx, ny, nz;
} SwirlPoint;

#define SWIRL_MAX_POINTS 2048
#define SWIRL_MAX_STRIP 512
#define SWIRL_ROUGHNESS 0.42f // lacquer: glossy, but wide enough for per-vertex highlights
#define SWIRL_F0 0.04f        // a dielectric's reflectance head on
#define SWIRL_TONE_STEPS 1024 // the tone curve's table, by the square root of radiance
#define SWIRL_TONE_SCALE 256.0f

// Linear albedo: the logo's orange #ED8432 and the European blue #1E55C0.
static const float sOrange[3] = { 0.846f, 0.231f, 0.032f };
static const float sBlue[3] = { 0.013f, 0.091f, 0.527f };

// Light colours as irradiance (times pi, folded out of the diffuse term).
#define SWIRL_LIGHTS 2
static const float sLightColour[SWIRL_LIGHTS][3] = {
    { 2.6f, 2.45f, 2.2f }, // key
    { 1.5f, 1.8f, 2.2f },  // rim
};
// The sky: irradiance from above and below, and its radiance at the zenith,
// at the horizon and under it, as reflected.
static const float sSkyAbove[3] = { 0.42f, 0.55f, 0.8f };
static const float sSkyBelow[3] = { 0.26f, 0.3f, 0.4f };
static const float sSkyZenith[3] = { 0.3f, 0.5f, 1.0f };
static const float sSkyHorizon[3] = { 0.95f, 1.0f, 1.05f };
static const float sSkyNadir[3] = { 0.35f, 0.45f, 0.6f };

// The glint: a narrow band of light that crosses the face as the swirl turns
// through square on to the camera, as the reflection of a light by the camera
// would, only slower. The band is a texture across the face, its texels a
// profile across the band; the PVR adds the lit colour (the offset colour)
// to the texel times the glint's colour.
#define GLINT_TEXELS 64          // across the band texture
#define GLINT_SPAN 40.0f         // model units the band texture spans
#define GLINT_CORE 0.12f         // half widths of the core and the light around
#define GLINT_SKIRT 0.45f        //   it, in the texture's width
#define GLINT_SKIRT_LEVEL 0.22f  // how bright the light around the core is
#define GLINT_WINDOW 0.84f       // tan 40 degrees: the band crosses while the eye is this close to square on
#define GLINT_REACH 76.0f        // model units out where it starts and ends, off the edge
#define GLINT_LEAN_X 0.94f       // the direction it crosses in (cos and sin of -20 degrees),
#define GLINT_LEAN_Y (-0.34f)    //   so the band leans to the right
#define GLINT_WARMUP 45          // frames shown before the first one: not while it fades in
#define GLINT_PEAK 0.55f         // its brightness, at the core when square on
#ifndef GLINT_TIMES
#define GLINT_TIMES 1000         // how many times it glints while shown (once a turn)
#endif
static const float sGlintColour[3] = { 1.0f, 0.96f, 0.88f }; // the key light's warm white

// Behind the swirl, the soft halo: its silhouette, blurred (make_swirl.py),
// on a square in its own plane through the middle, so it turns with it,
// added to the sky behind it in the translucent list. The settings in #ifndef
// (here and for the glint) can be given at build time to try variations.
#define HALO_PUSH 0.95f     // 1/w times this: always behind the swirl's body
#ifndef HALO_STRENGTH
#define HALO_STRENGTH 0.45f
#endif
#ifndef HALO_TOWARDS
#define HALO_TOWARDS 1.0f, 1.0f, 1.0f // the colour the halo is taken a little towards from the albedo
#endif
#ifndef HALO_WHITE
#define HALO_WHITE 0.3f     // how far towards it
#endif
#ifndef HALO_BREATHE
#define HALO_BREATHE 0.0f   // how far its strength ebbs and comes back, slowly (0: steady)
#endif
#define HALO_BREATH 240     // frames a breath takes
static const float sHaloTowards[3] = { HALO_TOWARDS };

static const ObjectModel *sSwirl; // as loaded, to know it when it is drawn
static float sAlbedo[3];
static u8 sTone[SWIRL_TONE_STEPS];
static u32 sArgb[SWIRL_MAX_POINTS];  // this frame's light, per distinct vertex
static u8 sOpacity;
// By opacity (opaque, translucent), then without and with the glint.
static pvr_poly_hdr_t sHeader[2][2] __attribute__((aligned(32)));
static pvr_poly_hdr_t sHaloHeader __attribute__((aligned(32)));
static pvr_vertex_t sStrip[SWIRL_MAX_STRIP] __attribute__((aligned(32)));
static float sScreen[SWIRL_MAX_POINTS][3];
static pvr_ptr_t sGlintTexture, sHaloTexture;
static u32 sHaloArgb;       // the halo's colour at full opacity
static int sDraws;          // frames drawn since the model was loaded
static int sGlintArmed;     // 1 once away from square on after fading in, -1 while one is under way
static int sGlints;         // glints since the model was loaded
static u32 sGlintArgb;      // this frame's glint colour, 0 for none
static float sGlintU[3];    // this frame's texture u: [0] + [1] x + [2] y

extern void pc_project_points(const f32 *xyz, int stride, int count, f32 (*out)[3]);
extern const f32 *pc_current_matrix(void);

static inline const SwirlCounts *counts(void) {
    return (const SwirlCounts *) sSwirlSurface;
}
static inline const SwirlPoint *points(void) {
    return (const SwirlPoint *) (sSwirlSurface + sizeof(SwirlCounts));
}
static inline const u16 *strip_lengths(void) {
    return (const u16 *) (points() + counts()->points);
}
static inline const u16 *strip_indices(void) {
    return strip_lengths() + counts()->strips;
}

/** ACES filmic (Narkowicz's fit), then sRGB as a square root. */
static void tone_init(void) {
    for (int i = 0; i < SWIRL_TONE_STEPS; i++) {
        float x = (i + 0.5f) / SWIRL_TONE_SCALE;

        x *= x;
        x = x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f);
        sTone[i] = x >= 1.0f ? 255 : (u8) (shz_sqrtf(x) * 255.0f);
    }
}

/** A grey RGB565 texel. */
static inline u16 grey565(float v) {
    int g = (int) (shz_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);

    return (u16) (((g * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | (g * 31 + 127) / 255);
}

/** w x h grey texels, from rows of bytes or a function of x, into video memory. */
static pvr_ptr_t texture(int w, int h, const u8 *bytes, float (*f)(float)) {
    u16 *texels = malloc(w * h * sizeof(u16));
    pvr_ptr_t out = NULL;

    if (texels != NULL) {
        for (int i = 0; i < w * h; i++) {
            texels[i] = grey565(bytes != NULL ? bytes[i] / 255.0f : f((i % w + 0.5f) / w - 0.5f));
        }
        out = pvr_mem_malloc(w * h * sizeof(u16));
        if (out != NULL) {
            pvr_txr_load(texels, out, w * h * sizeof(u16));
        }
        free(texels);
    }
    return out;
}

/** The glint across its band, x from -1/2 to 1/2: a narrow core in softer light. */
static float glint_profile(float x) {
    float core = 1.0f - (x / GLINT_CORE) * (x / GLINT_CORE);
    float skirt = 1.0f - (x / GLINT_SKIRT) * (x / GLINT_SKIRT);

    core = core > 0.0f ? core * core * core : 0.0f;
    skirt = skirt > 0.0f ? skirt * skirt * skirt : 0.0f;
    return core * (1.0f - GLINT_SKIRT_LEVEL) + skirt * GLINT_SKIRT_LEVEL;
}

static inline u32 argb(float r, float g, float b) {
    return 0xFF000000u | (u32) (shz_clampf(r, 0.0f, 1.0f) * 255.0f) << 16 |
           (u32) (shz_clampf(g, 0.0f, 1.0f) * 255.0f) << 8 | (u32) (shz_clampf(b, 0.0f, 1.0f) * 255.0f);
}

static void headers_init(void) {
    const u32 *glow = (const u32 *) sSwirlGlow;
    const int size = glow[0];
    pvr_poly_cxt_t cxt;

    // Once: they stay in video memory.
    if (sGlintTexture == NULL) {
        sGlintTexture = texture(GLINT_TEXELS, 8, NULL, glint_profile);
    }
    if (sHaloTexture == NULL) {
        sHaloTexture = texture(size, size, sSwirlGlow + 8, NULL);
    }
    for (int tr = 0; tr < 2; tr++) {
        for (int glint = 0; glint < 2; glint++) {
            if (glint) {
                pvr_poly_cxt_txr(&cxt, tr ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY,
                                 PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, GLINT_TEXELS, 8, sGlintTexture,
                                 PVR_FILTER_BILINEAR);
                cxt.gen.specular = PVR_SPECULAR_ENABLE; // the lit colour, added
                cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
                cxt.txr.uv_clamp = PVR_UVCLAMP_UV;      // dark either side of the band
            } else {
                pvr_poly_cxt_col(&cxt, tr ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
            }
            cxt.gen.shading = PVR_SHADE_GOURAUD;
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.clip_mode = PVR_USERCLIP_DISABLE;
            if (tr) {
                cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
            }
            pvr_poly_compile(&sHeader[tr][glint], &cxt);
        }
    }
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, size, size,
                     sHaloTexture, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_DISABLE;
    cxt.txr.env = PVR_TXRENV_MODULATE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_ONE; // sky + glow
    cxt.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&sHaloHeader, &cxt);
    // The albedo a little towards white, as sRGB.
    sHaloArgb = argb(HALO_STRENGTH * shz_sqrtf(sAlbedo[0] + (sHaloTowards[0] - sAlbedo[0]) * HALO_WHITE),
                     HALO_STRENGTH * shz_sqrtf(sAlbedo[1] + (sHaloTowards[1] - sAlbedo[1]) * HALO_WHITE),
                     HALO_STRENGTH * shz_sqrtf(sAlbedo[2] + (sHaloTowards[2] - sAlbedo[2]) * HALO_WHITE));
    sDraws = 0;
    sGlints = 0;
    sGlintArmed = 0;
}

/**
 * The size of the model the port supplies for modelID, or 0 to load it from
 * the assets. With out, also copies it there (big-endian, as from the disc).
 */
u32 pc_object_model_override(s32 modelID, void *out) {
    if (modelID != ASSET_OBJECTMODEL_N64LOGO) {
        return 0;
    }
    if (out != NULL) {
        memcpy(out, sSwirlModel, sizeof(sSwirlModel));
        sSwirl = out;
        memcpy(sAlbedo, flashrom_get_region() == FLASHROM_REGION_EUROPE ? sBlue : sOrange, sizeof(sAlbedo));
        tone_init();
        headers_init();
    }
    return sizeof(sSwirlModel);
}

static inline float pow5(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}

static inline u32 tone(float x) {
    int i;

    if (x <= 0.0f) {
        return 0;
    }
    i = (int) (shz_sqrtf_fsrra(x) * SWIRL_TONE_SCALE);
    return sTone[i < SWIRL_TONE_STEPS ? i : SWIRL_TONE_STEPS - 1];
}

/** w in the model's space, given its axes in world space. */
static inline shz_vec3_t to_model(shz_vec3_t w, const shz_vec3_t axes[3]) {
    return shz_vec3_init(shz_vec3_dot(w, axes[0]), shz_vec3_dot(w, axes[1]), shz_vec3_dot(w, axes[2]));
}

/** This frame's light at every distinct vertex, into sArgb. */
static void light(Object *obj, Camera *cam) {
    const SwirlPoint *pts = points();
    const u32 count = counts()->points;
    const float a = SWIRL_ROUGHNESS * SWIRL_ROUGHNESS;
    const float a2 = a * a;
    const float k = (SWIRL_ROUGHNESS + 1.0f) * (SWIRL_ROUGHNESS + 1.0f) * 0.125f;
    shz_vec3_t axes[3], origin, eye, fwd, right, up, sky, lights[SWIRL_LIGHTS];
    alignas(8) MtxF world;
    u32 i;

    // The model's axes and origin in world space; uniformly scaled, so a
    // world vector's model coordinates are its dot with each axis over the
    // scale squared.
    mtxf_from_transform(&world, &obj->trans);
    shz_xmtrx_load_4x4((const shz_mat4x4_t *) world);
    axes[0] = shz_xmtrx_transform_vec3(shz_vec3_init(1.0f, 0.0f, 0.0f));
    axes[1] = shz_xmtrx_transform_vec3(shz_vec3_init(0.0f, 1.0f, 0.0f));
    axes[2] = shz_xmtrx_transform_vec3(shz_vec3_init(0.0f, 0.0f, 1.0f));
    origin = shz_xmtrx_transform_point3(shz_vec3_init(0.0f, 0.0f, 0.0f));
    eye = shz_vec3_init(cam->trans.x_position, cam->trans.y_position, cam->trans.z_position);
    // The camera's frame, from where it looks: the lights keep to it.
    fwd = shz_vec3_normalize_safe(shz_vec3_sub(origin, eye));
    right = shz_vec3_normalize_safe(shz_vec3_cross(fwd, shz_vec3_init(0.0f, 1.0f, 0.0f)));
    up = shz_vec3_cross(right, fwd);
    lights[0] = shz_vec3_add(shz_vec3_add(shz_vec3_scale(fwd, -1.0f), shz_vec3_scale(up, 0.9f)),
                             shz_vec3_scale(right, -0.7f));
    lights[1] = shz_vec3_add(shz_vec3_add(shz_vec3_scale(fwd, 1.1f), shz_vec3_scale(up, 0.5f)),
                             shz_vec3_scale(right, 0.8f));
    for (i = 0; i < SWIRL_LIGHTS; i++) {
        lights[i] = shz_vec3_normalize(to_model(lights[i], axes));
    }
    sky = shz_vec3_normalize(to_model(shz_vec3_init(0.0f, 1.0f, 0.0f), axes));
    eye = shz_vec3_scale(to_model(shz_vec3_sub(eye, origin), axes), shz_invf(shz_vec3_dot(axes[0], axes[0])));

    for (i = 0; i < count; i++) {
        const SwirlPoint *pt = &pts[i];
        float nx = pt->nx, ny = pt->ny, nz = pt->nz;
        float vx = eye.x - pt->px, vy = eye.y - pt->py, vz = eye.z - pt->pz;
        float inv = shz_inv_sqrtf_fsrra(shz_dot6f(vx, vy, vz, vx, vy, vz));
        float nv, up_n, up_r, fe, gv, w, out[3];

        SHZ_PREFETCH(pt + 2);
        vx *= inv;
        vy *= inv;
        vz *= inv;
        nv = shz_dot6f(nx, ny, nz, vx, vy, vz);
        if (nv < 0.0f) {
            // Facing away, so seen only edge on: lit as if mirrored to face
            // the eye, so the silhouette doesn't go black.
            nx -= 2.0f * nv * vx;
            ny -= 2.0f * nv * vy;
            nz -= 2.0f * nv * vz;
            nv = -nv;
        }
        if (nv < 1e-3f) {
            nv = 1e-3f;
        }
        // The sky: diffuse from the hemisphere around the normal, specular
        // from where it reflects to, by Fresnel with roughness.
        up_n = shz_dot6f(nx, ny, nz, sky.x, sky.y, sky.z);
        up_r = 2.0f * nv * up_n - shz_dot6f(vx, vy, vz, sky.x, sky.y, sky.z);
        fe = SWIRL_F0 + ((1.0f - SWIRL_ROUGHNESS) - SWIRL_F0) * pow5(1.0f - nv);
        w = 0.5f + 0.5f * up_n;
        for (int c = 0; c < 3; c++) {
            float reflected;

            if (up_r >= 0.0f) {
                float t = (1.0f - up_r) * (1.0f - up_r);
                reflected = sSkyZenith[c] + (sSkyHorizon[c] - sSkyZenith[c]) * t;
            } else {
                reflected = sSkyHorizon[c] + (sSkyNadir[c] - sSkyHorizon[c]) * -up_r;
            }
            out[c] = sAlbedo[c] * (sSkyBelow[c] + (sSkyAbove[c] - sSkyBelow[c]) * w) * (1.0f - fe) + reflected * fe;
        }
        // The lights. Specular: pi * D * G / (4 nl nv) * F, with D's pi
        // cancelled, Smith's terms nl (1 - k) + k and nv (1 - k) + k.
        gv = nv * (1.0f - k) + k;
        for (int l = 0; l < SWIRL_LIGHTS; l++) {
            float lx = lights[l].x, ly = lights[l].y, lz = lights[l].z;
            float nl = shz_dot6f(nx, ny, nz, lx, ly, lz);
            float hx, hy, hz, nh, vh, d, f, spec;

            if (nl <= 0.0f) {
                continue;
            }
            hx = lx + vx;
            hy = ly + vy;
            hz = lz + vz;
            inv = shz_inv_sqrtf_fsrra(shz_dot6f(hx, hy, hz, hx, hy, hz));
            nh = shz_dot6f(nx, ny, nz, hx, hy, hz) * inv;
            vh = shz_dot6f(vx, vy, vz, hx, hy, hz) * inv;
            if (nh < 0.0f) {
                nh = 0.0f;
            }
            if (vh < 0.0f) {
                vh = 0.0f;
            }
            d = nh * nh * (a2 - 1.0f) + 1.0f;
            f = SWIRL_F0 + (1.0f - SWIRL_F0) * pow5(1.0f - vh);
            spec = 0.25f * a2 * f * shz_invf_fsrra(d * d * (nl * (1.0f - k) + k) * gv);
            for (int c = 0; c < 3; c++) {
                out[c] += sLightColour[l][c] * (sAlbedo[c] * (1.0f - f) + spec) * nl;
            }
        }
        sArgb[i] = 0xFF000000u | (tone(out[0]) << 16) | (tone(out[1]) << 8) | tone(out[2]);
    }
}

/**
 * In render_3d_model, for each model it draws: 1 if it is the swirl, lit and
 * left to the display-list walk; 0 to have the game draw the model.
 */
int pc_swirl_render(Object *obj, ObjectModel *model, Gfx **dl, s32 opacity) {
    Camera *cam = cam_get_active_camera();

    if (model != sSwirl || cam == NULL || counts()->points > SWIRL_MAX_POINTS) {
        return 0;
    }
    light(obj, cam);
    sOpacity = opacity < 0 ? 0 : opacity > 255 ? 255 : opacity;
    gDPNoOpTag((*dl)++, PC_SWIRL_TAG);
    return 1;
}

/**
 * The eye in the model's space, from the matrix it is drawn with: the point
 * that clip x, y and w all take to 0.
 */
static shz_vec3_t model_eye(void) {
    const f32 *m = pc_current_matrix(); // column-major: row r is m[r], m[4 + r], m[8 + r]
    shz_vec3_t rx = shz_vec3_init(m[0], m[4], m[8]);
    shz_vec3_t ry = shz_vec3_init(m[1], m[5], m[9]);
    shz_vec3_t rw = shz_vec3_init(m[3], m[7], m[11]);
    shz_vec3_t cx = shz_vec3_cross(ry, rw), cy = shz_vec3_cross(rw, rx), cw = shz_vec3_cross(rx, ry);

    // Cramer's rule for rx.e = -m[12], ry.e = -m[13], rw.e = -m[15].
    return shz_vec3_scale(shz_vec3_add(shz_vec3_add(shz_vec3_scale(cx, m[12]), shz_vec3_scale(cy, m[13])),
                                       shz_vec3_scale(cw, m[15])),
                          -shz_invf(shz_vec3_dot(rx, cx)));
}

/**
 * This frame's glint, from where the eye is: while it is within the window
 * either side of square on to the face, the band lies across the face where
 * the eye is, as the reflection of a light by the camera would, but stretched
 * so it takes the whole window to cross, and is brightest square on. Once a
 * turn, after the swirl has faded in and been away from square on.
 */
static void glint_update(shz_vec3_t eye) {
    float t, g;

    sGlintArgb = 0;
    if (eye.z <= 0.0f || shz_fabsf(eye.x) >= GLINT_WINDOW * eye.z) {
        if (sGlintArmed < 0) {
            sGlints++; // one just ended
        }
        sGlintArmed = sDraws > GLINT_WARMUP && sGlints < GLINT_TIMES;
        return;
    }
    if (!sGlintArmed) {
        return;
    }
    sGlintArmed = -1; // under way
    t = eye.x * shz_invf(GLINT_WINDOW * eye.z); // -1 to 1 across the window
    g = GLINT_PEAK * (1.0f - t * t);
    sGlintArgb = argb(sGlintColour[0] * g, sGlintColour[1] * g, sGlintColour[2] * g) & 0x00FFFFFFu;
    // u = 1/2 + (lean . (x, y) - REACH t) / SPAN
    sGlintU[0] = 0.5f - GLINT_REACH * t / GLINT_SPAN;
    sGlintU[1] = GLINT_LEAN_X / GLINT_SPAN;
    sGlintU[2] = GLINT_LEAN_Y / GLINT_SPAN;
}

/** The halo: a square of the blurred silhouette in the swirl's plane, behind it. */
static void halo_draw(void) {
    const float e = ((const float *) sSwirlGlow)[1];
    float corner[4][3], screen[4][3];
    pvr_vertex_t quad[4] __attribute__((aligned(32)));
    u32 colour = sHaloArgb, level = sOpacity;

    if (sHaloTexture == NULL) {
        return;
    }
    if (HALO_BREATHE != 0.0f) {
        // Slowly, from its full strength down and back: 1 - BREATHE (1 - cos) / 2.
        float c = shz_cosf((float) (sDraws % HALO_BREATH) * (2.0f * SHZ_F_PI / HALO_BREATH));

        level = (u32) (sOpacity * (1.0f - HALO_BREATHE * 0.5f * (1.0f - c)));
    }
    if (level < 255) {
        u32 r = (colour >> 16 & 0xFF) * level / 255, g = (colour >> 8 & 0xFF) * level / 255,
            b = (colour & 0xFF) * level / 255;
        colour = 0xFF000000u | r << 16 | g << 8 | b;
    }
    for (int i = 0; i < 4; i++) {
        // Top left, top right, bottom left, bottom right.
        corner[i][0] = i & 1 ? e : -e;
        corner[i][1] = i & 2 ? -e : e;
        corner[i][2] = 0.0f;
    }
    pc_project_points(corner[0], 3, 4, screen);
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t *v = &quad[i];

        if (screen[i][2] <= 0.0f) {
            return;
        }
        v->flags = i < 3 ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
        v->x = screen[i][0];
        v->y = screen[i][1];
        v->z = HALO_PUSH * shz_invf(screen[i][2]);
        v->u = i & 1 ? 1.0f : 0.0f;
        v->v = i & 2 ? 1.0f : 0.0f;
        v->argb = colour;
        v->oargb = 0;
    }
    gfx_tr_native(&sHaloHeader, quad, 4);
}

void pc_swirl_draw(void) {
    const SwirlCounts *n = counts();
    const SwirlPoint *pts = points();
    const u16 *lengths = strip_lengths();
    const u16 *index = strip_indices();
    int translucent = sOpacity < 255;
    const pvr_poly_hdr_t *hdr;
    u32 alpha = (u32) sOpacity << 24;
    u32 s;

    sDraws++;
    glint_update(model_eye());
    halo_draw();
    hdr = &sHeader[translucent][sGlintArgb != 0];
    pc_project_points(&pts->px, sizeof(SwirlPoint) / sizeof(float), n->points, sScreen);
    for (s = 0; s < n->strips; s++) {
        int len = lengths[s];
        int ok;

        if (len > SWIRL_MAX_STRIP) {
            index += len;
            continue;
        }
        for (int j = 0; j < len; j++) {
            const float *p = sScreen[index[j]];
            pvr_vertex_t *v = &sStrip[j];

            v->flags = j + 1 < len ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
            v->x = p[0];
            v->y = p[1];
            v->z = shz_invf_fsrra(p[2]);
            v->v = 0.5f;
            if (sGlintArgb) {
                // The glint's colour times the band, plus the light as the offset.
                const SwirlPoint *pt = &pts[index[j]];

                v->u = sGlintU[0] + sGlintU[1] * pt->px + sGlintU[2] * pt->py;
                v->argb = sGlintArgb | alpha;
                v->oargb = sArgb[index[j]];
            } else {
                v->u = 0.0f;
                v->argb = (sArgb[index[j]] & 0x00FFFFFFu) | alpha;
                v->oargb = 0;
            }
        }
        index += len;
        ok = translucent ? gfx_tr_native(hdr, sStrip, len) : gfx_op_native(hdr, sStrip, len);
        if (!ok) {
            return;
        }
        hdr = NULL;
    }
}
