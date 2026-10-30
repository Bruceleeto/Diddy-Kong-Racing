#ifndef DKR_NATIVE_GAME_H
#define DKR_NATIVE_GAME_H

// The native renderer as the game's code sees it (dreamcast/native.c), without
// the console's headers, which do not mix with the game's.
#include "types.h"
#include "structs.h"

#if defined(TARGET_DC) && defined(DKR_NATIVE) && DKR_NATIVE
typedef struct NatMaterial NatMaterial;
const NatMaterial *pc_nat_material(TextureHeader *tex, s32 flags, s32 texOffset);
const NatMaterial *pc_nat_material_state(TextureHeader *tex, const Gfx *state, s32 zBuffer, s32 fog);
s32 pc_nat_sprite(Gfx **dl, const Sprite *sprite, s32 frame, s32 flags, const Vertex *anchor, const Mtx *billboard);
// A texture rectangle over the colours and modes the display list has set; FALSE
// if its texture is not one the texture pack holds.
s32 pc_nat_rect(Gfx **dl, TextureHeader *tex, s32 ulx, s32 uly, s32 lrx, s32 lry, s32 s, s32 t, s32 dsdx, s32 dtdy);
void pc_nat_tris(Gfx **dl, const NatMaterial *mat, const Vertex *verts, s32 numVerts, const Triangle *tris, s32 numTris);

/**
 * material_set, gSPVertexDKR and gSPPolygon in one. FALSE if the display list
 * has to draw it after all. The material comes from the game's texture and
 * flags; the matrix and the colours are those the display list has set by then.
 */
static inline s32 nat_mesh(Gfx **dl, TextureHeader *tex, s32 flags, s32 texOffset, const Vertex *verts, s32 numVerts,
                           const Triangle *tris, s32 numTris) {
    const NatMaterial *mat;

    if (numVerts > 32 || numTris > 16) {
        return FALSE;
    }
    mat = pc_nat_material(tex, flags, texOffset);
    if (mat == NULL) {
        return FALSE;
    }
    pc_nat_tris(dl, mat, verts, numVerts, tris, numTris);
    return TRUE;
}
#else
#define nat_mesh(dl, tex, flags, texOffset, verts, numVerts, tris, numTris) FALSE
#define pc_nat_sprite(dl, sprite, frame, flags, anchor, billboard) FALSE
#define pc_nat_rect(dl, tex, ulx, uly, lrx, lry, s, t, dsdx, dtdy) FALSE
#endif

#endif
