/* Decompiled from src/hasm/ido/obj_shade_fast.s (hand-written asm in retail).
 * Cheap per-vertex dynamic lighting for objects: dots each vertex normal
 * against a light direction and writes greyscale vertex colours. The fancier
 * sibling (obj_shade_fancy / calc_dynamic_lighting_for_object_1 in objects.c)
 * follows the same batch-walking structure. */
#include "camera.h"
#include "math_util.h"
#include "objects.h"
#include "structs.h"
#include "textures_sprites.h"
#include "types.h"

/**
 * Shade an object's vertices from its shadow light direction, without
 * rotating the light into object space first (the direction is used as-is).
 * Used for AI racers and multiplayer views, where the cost of the full
 * version isn't worth it.
 * Batches using vertex colours (BATCH_VTX_COL) are skipped; their normals are
 * only consumed if the batch is environment-mapped.
 */
void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    s32 shade;
    s32 base;
    s16 normIdx;
    s16 i;
    s16 j;
    s32 dirX, dirY, dirZ;
    Vertex *vertices;
    Vec3s *normals;
    ShadeProperties *shading;

    shading = obj->shading;
    if (shading == NULL) {
        return;
    }

    dirX = shading->shadowDirX;
    dirY = shading->shadowDirY;
    dirZ = shading->shadowDirZ;
    vertices = obj->curVertData;
    normals = model->normals;
    normIdx = 0;
    base = shading->unk0 * intensity * 160.0f;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (model->batches[i].miscData != BATCH_VTX_COL) {
            for (j = model->batches[i].verticesOffset; j < model->batches[i + 1].verticesOffset; j++) {
                shade = (normals[normIdx].x * dirX + normals[normIdx].y * dirY + normals[normIdx].z * dirZ) >> 11;
                if (shade > 0) {
                    shade = ((shade * base) >> 16) + base;
                    if (shade > 255) {
                        shade = 255;
                    }
                } else {
                    shade = base;
                }
                vertices[j].r = shade;
                vertices[j].g = shade;
                vertices[j].b = shade;
                vertices[j].a = 255;
                normIdx++;
            }
        } else if (model->batches[i].flags & RENDER_ENVMAP) {
            normIdx += model->batches[i + 1].verticesOffset - model->batches[i].verticesOffset;
        }
    }
}

/**
 * The dynamic ambient lighting half of obj_shade_fancy's pair (see objects.c).
 * Rotates the shadow light direction into object space (optionally through
 * the projection matrix first, when arg2 is set), then shades each vertex
 * with an ambient + diffuse greyscale term.
 * Used for racers, the Rare logo, Wizpig's face, etc.
 */
void calc_dynamic_lighting_for_object_2(Object *obj, ObjectModel *model, s16 arg2, f32 intensity) {
    Vec3f direction;
    ObjectTransform trans;
    MtxF mtx;
    ShadeProperties *shading;
    Vertex *vertices;
    Vec3s *normals;
    s32 dirX, dirY, dirZ;
    s32 ambientFactor;
    s32 diffuseFactor;
    f32 colourBase;
    s32 shade;
    s16 normIdx;
    s16 i;
    s16 j;

    shading = obj->shading;
    if (shading == NULL) {
        return;
    }

    direction.x = shading->shadowDirX << 2;
    direction.y = shading->shadowDirY << 2;
    direction.z = shading->shadowDirZ << 2;
    if (arg2) {
        mtxf_transform_dir(get_projection_matrix_f32(), &direction, &direction);
    }

    trans.rotation.y_rotation = -obj->trans.rotation.y_rotation;
    trans.rotation.x_rotation = -obj->trans.rotation.x_rotation;
    trans.rotation.z_rotation = -obj->trans.rotation.z_rotation;
    trans.scale = 1.0f;
    trans.x_position = 0.0f;
    trans.y_position = 0.0f;
    trans.z_position = 0.0f;
    mtxf_from_inverse_transform(&mtx, &trans);
    mtxf_transform_dir(&mtx, &direction, &direction);

    colourBase = shading->unk0 * intensity * 255.0f;
    ambientFactor = shading->ambient * colourBase;
    diffuseFactor = shading->diffuse * colourBase;
    dirX = direction.x;
    dirY = direction.y;
    dirZ = direction.z;
    vertices = obj->curVertData;
    normals = model->normals;
    normIdx = 0;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (model->batches[i].miscData != BATCH_VTX_COL) {
            for (j = model->batches[i].verticesOffset; j < model->batches[i + 1].verticesOffset; j++) {
                shade = (normals[normIdx].x * dirX + normals[normIdx].y * dirY + normals[normIdx].z * dirZ) >> 7;
                if (shade > 0) {
                    shade = ((shade * diffuseFactor) >> 21) + ambientFactor;
                    if (shade > 255) {
                        shade = 255;
                    }
                } else {
                    shade = ambientFactor;
                }
                vertices[j].r = shade;
                vertices[j].g = shade;
                vertices[j].b = shade;
                vertices[j].a = 255;
                normIdx++;
            }
        } else if (model->batches[i].flags & RENDER_ENVMAP) {
            normIdx += model->batches[i + 1].verticesOffset - model->batches[i].verticesOffset;
        }
    }
}
