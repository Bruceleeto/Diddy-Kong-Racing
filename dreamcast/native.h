#ifndef DKR_NATIVE_H
#define DKR_NATIVE_H

// The native renderer: draws from what dreamcast/build_native_meshes.py compiled
// offline, with no display list in between. See dreamcast/native.c.
#include <dc/pvr.h>
#include <sh4zam/shz_sh4zam.h>

#include "gfx.h"
#include "types.h"

// A native draw has its place in the frame's display list, for as long as
// parts of the frame are still drawn by the walk: one command, which the walk
// hands straight to dreamcast/native.c. Unused by the F3D microcode DKR ran.
#define G_NATIVE 0x0A
#define NATIVE_LEVEL 0 // w0 low half: segment, bit 16: water hidden; w1: chunks first, end
#define NATIVE_CHECK_LEVEL 1 // NATIVE_CHECK=1: w0 low half: segment; w1: batch
#define NATIVE_MODEL 2 // w0: first batch, variant, vertices split; w1: vertices; then a word with w1 the model
#define NATIVE_CHECK_MODEL 3 // NATIVE_CHECK=1: w0: batch, variant; w1: the model
#define NATIVE_SPRITE 5 // w0: frame, z buffer; w1: sprite; then words with the anchor, the matrix, the render table's entry
#define NATIVE_TRIS 4 // w0: vertices, triangles; w1: vertices; then a word with the material and the triangles

#define gDkrNative(pkt, kind, a, b)                                                                     \
    do {                                                                                                \
        Gfx *_g = (Gfx *) (pkt);                                                                        \
        _g->words.w0 = ((u32) G_NATIVE << 24) | ((u32) (kind) << 20) | (u32) (a);                       \
        _g->words.w1 = (u32) (b);                                                                       \
    } while (0)

// What a frame's chunk draws through.
typedef struct {
    const f32 *matrix;    // the 4x4 the positions go through, columns first
    const f32 *matrix2;   // a racer's head (slot 2)
    u32 prim, env;        // as the combiner keys them, RGBA
    f32 vpScaleX, vpScaleY, vpTransX, vpTransY; // the N64 viewport
    s32 serial;           // changes when any of the above does
} NatView;

// One compiled corner: what the PVR gets for it, all but the position.
typedef struct {
    f32 u, v;
    u32 argb;
    u32 word; // the PVR command in the top nibble, the position's index in the low 16 bits
} NatCorner;

/**
 * shz_xmtrx_transform_vec4 on fv4 rather than fv8, with w = 1: a second vector
 * register, so two vertices can be in the transform at once.
 */
static inline shz_vec4_t nat_transform_fv4(f32 x, f32 y, f32 z) {
    register f32 rx __asm__("fr4") = x;
    register f32 ry __asm__("fr5") = y;
    register f32 rz __asm__("fr6") = z;
    register f32 rw __asm__("fr7") = 1.0f;

    __asm__ __volatile__("ftrv xmtrx, fv4" : "+f"(rx), "+f"(ry), "+f"(rz), "+f"(rw));
    return shz_vec4_init(rx, ry, rz, rw);
}

/** One PVR vertex, whole: to a store queue or, with alloc, to a cache line it does not read first. */
static inline void nat_vertex_put(pvr_vertex_t *vt, s32 alloc, u32 flags, f32 x, f32 y, f32 z, f32 u, f32 v, u32 argb,
                                  u32 oargb) {
    u8 *top = (u8 *) vt + 32;

    if (alloc) {
        __asm__ __volatile__("movca.l r0, @%1" : "=m"(*vt) : "r"(vt), "z"(flags));
    }
    __asm__ __volatile__("mov.l %[oargb], @-%[p]\n\t"
                         "mov.l %[argb], @-%[p]\n\t"
                         "fmov.s %[v], @-%[p]\n\t"
                         "fmov.s %[u], @-%[p]\n\t"
                         "fmov.s %[z], @-%[p]\n\t"
                         "fmov.s %[y], @-%[p]\n\t"
                         "fmov.s %[x], @-%[p]\n\t"
                         "mov.l %[flags], @-%[p]"
                         : [p] "+r"(top), "+m"(*vt)
                         : [oargb] "r"(oargb), [argb] "r"(argb), [v] "f"(v), [u] "f"(u), [z] "f"(z), [y] "f"(y),
                           [x] "f"(x), [flags] "r"(flags));
}

/** Send a store queue's vertex to the TA. The asm reads the vertex, which orders the stores before it. */
static inline void nat_sq_flush(pvr_vertex_t *vt) {
    __asm__ __volatile__("pref @%0" : : "r"(vt), "m"(*vt));
}

// The level (src/tracks.c).
void pc_nat_level_load(s32 modelId);
void pc_nat_level_free(void);
void pc_nat_vram_lost(void);
// Batch `batch` of a segment on: 0 if the walk has to draw it; otherwise how many
// batches, from it, are done by the G_NATIVE command this adds to *dl.
s32 pc_nat_level_batches(Gfx **dl, s32 segment, s32 batch, s32 end, s32 waterHidden);
void pc_nat_check_level(Gfx **dl, s32 segment, s32 batch);

// Object models (src/object_models.c, src/objects.c). render_mesh's return, or -2
// if the walk has to draw it.
void pc_nat_model_load(ObjectModel *model, s32 modelId);
void pc_nat_model_free(ObjectModel *model);
s32 pc_nat_model_draw(Gfx **dl, ObjectModel *model, const Vertex *verts, s32 start, s32 flags, s32 overrideVerts);
void pc_nat_check_model(Gfx **dl, ObjectModel *model, s32 batch, s32 flags);

// What a frame makes up: particles, shadows, the water. The material for
// material_set's arguments, or for two commands of the game's own and its
// geometry mode; NULL if the walk has to draw it.
typedef struct NatMaterial NatMaterial;
const NatMaterial *pc_nat_material(TextureHeader *tex, s32 flags, s32 texOffset);
const NatMaterial *pc_nat_material_state(TextureHeader *tex, const Gfx *state, s32 zBuffer, s32 fog);
void pc_nat_tris(Gfx **dl, const NatMaterial *mat, const Vertex *verts, s32 numVerts, const Triangle *tris, s32 numTris);
void pc_nat_materials_forget(void);
s32 pc_nat_sprite(Gfx **dl, const Sprite *sprite, s32 frame, s32 flags, const Vertex *anchor, const Mtx *billboard);

// The walk's side (dreamcast/main.c).
void pc_nat_level_run(const NatView *view, u32 w0, u32 w1);
void pc_nat_check_level_run(u32 w0, u32 w1, const u32 *colours, s32 count);
void pc_nat_model_run(const NatView *view, u32 w0, u32 w1, u32 w1b);
void pc_nat_check_model_run(u32 w0, u32 w1);
void pc_nat_sprite_run(const NatView *view, u32 w0, u32 w1, const Gfx *next);
void pc_nat_tris_run(const NatView *view, u32 w0, u32 w1, const NatMaterial *mat, const Triangle *tris);
// How a combiner treats the shade under these colours. TRUE: a colour is
// (shade & keep) | konst. FALSE: pc_nat_colours_mux does it, vertex by vertex.
s32 pc_nat_classify(u32 w0, u32 w1, u32 cycle, u32 prim, u32 env, u32 *keep, u32 *konst);
void pc_nat_colours_mux(u32 *argb, const Vertex *verts, s32 count);

// The texture a frame's pixels went to the PVR as: the bits of the polygon header
// the texture decides. 0 if the texture is not in video memory.
s32 pc_nat_texture_words(const void *textureHeader, u32 *mode2, u32 *mode3);
void pc_nat_texture_scale(const void *textureHeader, f32 *u, f32 *v);
// `clip`: the clip space positions; `argb`: their colours, or NULL for the corners' own.
pvr_vertex_t *pc_nat_clip(const NatCorner *corners, s32 count, const shz_vec4_t *clip, const u32 *argb, s32 cull,
                          pvr_vertex_t *record, f32 scaleX, f32 scaleY, f32 zMul, u32 offset, s32 textured);

#endif
