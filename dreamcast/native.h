#ifndef DKR_NATIVE_H
#define DKR_NATIVE_H

// The native renderer: draws from what dreamcast/build_native_meshes.py compiled
// offline, with no display list in between. See dreamcast/native.c.
#include <dc/pvr.h>
#include <sh4zam/shz_sh4zam.h>

#include "gfx.h"
#include "types.h"

typedef struct NatMaterial NatMaterial;

// A texture as a draw needs it.
typedef struct {
    u32 handle;                // gfx.c's, 0 for none
    const void *header;        // the game's, if it came from one
    u16 width, height;
    u8 cms, cmt;
} NatTexture;

// What the game has set: a list's state, for the draws it makes.
typedef struct {
    const f32 *matrix[3];      // the Mtx of each slot
    const f32 *matrixV, *matrixV2; // each vertex load's, as it was loaded
    const void *verts, *verts2;
    s32 numVerts, numVerts2;
    s32 cur, billboard;
    u32 prim, env, blend, fill;
    u32 combine0, combine1;
    u32 otherH, otherL, geometry;
    NatTexture texture;
    const void *lastHeader;
} NatState;

extern NatState gNat;

enum {
    NAT_REC_LEVEL,      // a: segment and water hidden, b: first chunk, c: end chunk
    NAT_REC_MODEL,      // a: first batch, variant, split; p0: vertices; p1: model
    NAT_REC_SPRITE,     // a: frame, z buffer; p0: sprite; p1: anchor (or NULL); p2: render table entry
    NAT_REC_TRIS,       // mat; p0, a: vertices; p1, b: a second load; p2, c: triangles
    NAT_REC_RECT,       // mat; f: corners; a, b: texture coordinates
    NAT_REC_FILL,       // mat; f: corners; b: fill colour
    NAT_REC_VIEWPORT,   // p0
    NAT_REC_SCISSOR,    // f
    NAT_REC_FOG_COLOUR, // b
    NAT_REC_FOG_DEPTH,  // b: as gSPFogPosition packs it
    NAT_REC_TAG         // b
};

// One thing to draw, with all it needs from the game's state.
typedef struct {
    u8 kind;
    union {
        u8 fog;
        u8 run; // NAT_REC_RECT: the rectangles in this one's run, the next records
    };
    u8 place, layer; // a draw without a depth test: GFX_PLACE_*, covers before it
    u32 prim, env;
    const f32 *matrix, *matrix2;
    const NatMaterial *mat;
    union {
        struct {
            u32 a, b, c;
            const void *p0, *p1, *p2;
        };
        struct {
            u32 pad[2];
            f32 f[4];
        };
    };
} NatRec;

// The lists, in the pack's numbering.
enum { NAT_LIST_OP, NAT_LIST_PT, NAT_LIST_TR };

#define NAT_RECORDS 4096
#define NAT_RECT_RUN 32
// The builds that show the last frame again while the game stands still.
#define NAT_FRAME_REPLAY (DKR_DEBUG_VIEW || DKR_DEBUG_MENU)
NatRec *pc_nat_record(s32 kind);
void pc_nat_frame_reset(void);
void nat_ignore(s32 ignore);
// Into the bin of a list. A draw under a depth test says so with pc_nat_depth_drawn;
// one without takes pc_nat_bin_2d, which needs its alpha and its texture only if
// pc_nat_may_open.
void pc_nat_bin(s32 list, NatRec *rec);
void pc_nat_depth_drawn(void);
s32 pc_nat_may_open(void);
s32 pc_nat_view_backdrop(void);
void pc_nat_bin_2d(NatRec *rec, s32 alphaTest, s32 alpha, u32 handle);
void pc_nat_bin_tris(NatRec *rec);
s32 pc_nat_rect_alpha(const NatRec *rec);
void pc_nat_frame_requeue(void); // NAT_FRAME_REPLAY: the last frame's bins again
void pc_nat_planes_forget(void);
void pc_nat_sky(s32 sky);
// enDjinn's renderer of a record, and what a list starts from.
void pc_nat_item(void *data);
void pc_nat_list_begin(pvr_list_t list);

// What a record draws through, as the frame goes out.
typedef struct {
    const f32 *matrix;    // the 4x4 the positions go through, columns first
    const f32 *matrix2;   // a racer's head (slot 2)
    u32 prim, env;        // as the combiner keys them, RGBA
    f32 vpScaleX, vpScaleY, vpTransX, vpTransY; // the N64 viewport
    s32 exact;            // the scissor is not on the tiles
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

/** One PVR vertex, whole, to a store queue. */
static inline void nat_vertex_put(pvr_vertex_t *vt, u32 flags, f32 x, f32 y, f32 z, f32 u, f32 v, u32 argb, u32 oargb) {
    u8 *top = (u8 *) vt + 32;

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
void pc_nat_materials_forget(void);
// Batch `batch` of a segment on: 0 if the game has to draw it; otherwise how many
// batches, from it, the record this adds draws.
s32 pc_nat_level_batches(s32 segment, s32 batch, s32 end, s32 waterHidden);

// Object models (src/object_models.c, src/objects.c). render_mesh's return, or -2
// if the batch has to go the way of any other mesh.
void pc_nat_model_load(ObjectModel *model, s32 modelId);
void pc_nat_model_free(ObjectModel *model);
s32 pc_nat_model_draw(ObjectModel *model, const Vertex *verts, s32 start, s32 flags, s32 overrideVerts);

// The frame's side (dreamcast/main.c).
void pc_nat_level_run(const NatView *view, const NatRec *rec);
const NatMaterial *pc_nat_material_now(s32 textured);
void pc_nat_texture_of(const void *textureHeader, NatTexture *out);
void pc_nat_texture_image(const void *image, s32 fmt, s32 siz, s32 width, s32 height, s32 cms, s32 cmt, s32 swapped,
                          NatTexture *out);
s32 pc_nat_sprite(const Sprite *sprite, s32 frame, s32 flags, const Mtx *billboard);
void pc_nat_run_viewport(const void *viewport, NatView *view);
void pc_nat_run_scissor(const f32 *f);
void pc_nat_run_fog_colour(u32 rgba);
void pc_nat_run_fog_depth(u32 data);
void pc_nat_run_tag(u32 tag, const f32 *matrix);
void pc_nat_run_rects(const NatRec *rec);
void pc_nat_run_fill(const NatRec *rec);
void pc_nat_draw_tris(const NatMaterial *mat, const GfxTriVert *verts, s32 count);
u32 pc_nat_material_handle(const NatMaterial *mat);
void pc_nat_model_run(const NatView *view, const NatRec *rec);
void pc_nat_sprite_run(const NatView *view, const NatRec *rec);
void pc_nat_tris_run(const NatView *view, const NatRec *rec);
// How a combiner treats the shade under these colours. TRUE: a colour is
// (shade & keep) | konst. FALSE: pc_nat_colours_mux does it, vertex by vertex.
s32 pc_nat_classify(u32 w0, u32 w1, u32 cycle, u32 prim, u32 env, u32 *keep, u32 *konst);
void pc_nat_load_state(const NatMaterial *mat, u32 prim, u32 env);
void pc_nat_set_modes(u32 w0, u32 w1, u32 otherH, u32 otherL, u32 prim, u32 env);
void pc_nat_material_size(const NatMaterial *mat, s32 *w, s32 *h, s32 *cms, s32 *cmt);
void pc_nat_material_flags(const NatMaterial *mat, s32 *depth, s32 *write, s32 *decal, s32 *blend, s32 *alpha, s32 *fog, s32 *point);
void pc_nat_colours_mux(u32 *argb, const Vertex *verts, s32 count);

// The texture a frame's pixels went to the PVR as: the bits of the polygon header
// the texture decides. 0 if the texture is not in video memory.
s32 pc_nat_texture_words(const void *textureHeader, u32 *mode2, u32 *mode3);
void pc_nat_texture_scale(const void *textureHeader, f32 *u, f32 *v);
u32 pc_nat_texture_handle(const void *textureHeader);
// `clip`: the clip space positions; `argb`: their colours, or NULL for the corners' own.
void pc_nat_clip(const NatCorner *corners, s32 count, const shz_vec4_t *clip, const u32 *argb, s32 cull, f32 scaleX,
                 f32 scaleY, f32 zMul, u32 offset, s32 textured);

#endif
