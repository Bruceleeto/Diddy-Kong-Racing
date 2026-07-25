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

#ifdef TARGET_DC
#   include <sh4zam/shz_sh4zam.h>
#endif

/**
 * Shade an object's vertices from its shadow light direction, without
 * rotating the light into object space first (the direction is used as-is).
 * Used for AI racers and multiplayer views, where the cost of the full
 * version isn't worth it.
 * Batches using vertex colours (BATCH_VTX_COL) are skipped; their normals are
 * only consumed if the batch is environment-mapped.
 */
void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    s16 normIdx;
    s16 i;
    s16 j;
    Vertex *vertices;
    Vec3s *normals;
    ShadeProperties *shading;
#ifdef TARGET_DC
    f32 dirXf, dirYf, dirZf;
    f32 baseF;
    f32 scale;
#else
    s32 shade;
    s32 base;
    s32 dirX, dirY, dirZ;
#endif

    shading = obj->shading;
    if (shading == NULL) {
        return;
    }

#ifdef TARGET_DC
    dirXf = (f32) shading->shadowDirX;
    dirYf = (f32) shading->shadowDirY;
    dirZf = (f32) shading->shadowDirZ;
    baseF = shading->unk0 * intensity * 160.0f;
    scale = baseF / 134217728.0f; /* 2^27 == 2^(11+16), combining the >>11 and >>16 from the original chain */
#else
    dirX = shading->shadowDirX;
    dirY = shading->shadowDirY;
    dirZ = shading->shadowDirZ;
    base = shading->unk0 * intensity * 160.0f;
#endif
    vertices = obj->curVertData;
    normals = model->normals;
    normIdx = 0;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (model->batches[i].miscData != BATCH_VTX_COL) {
            for (j = model->batches[i].verticesOffset; j < model->batches[i + 1].verticesOffset; j++) {
#ifdef TARGET_DC
                f32 dot = shz_dot6f((f32) normals[normIdx].x, (f32) normals[normIdx].y, (f32) normals[normIdx].z,
                                    dirXf, dirYf, dirZf);
                f32 shadeF;
                if (dot > 0.0f) {
                    shadeF = dot * scale + baseF;
                    if (shadeF > 255.0f) {
                        shadeF = 255.0f;
                    }
                } else {
                    shadeF = baseF;
                }
                vertices[j].r = (u8) shadeF;
                vertices[j].g = (u8) shadeF;
                vertices[j].b = (u8) shadeF;
#else
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
#endif
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
    f32 colourBase;
    s16 normIdx;
    s16 i;
    s16 j;
#ifdef TARGET_DC
    f32 ambientFactorF;
    f32 diffuseScale;
#else
    s32 dirX, dirY, dirZ;
    s32 ambientFactor;
    s32 diffuseFactor;
    s32 shade;
#endif

    shading = obj->shading;
    if (shading == NULL) {
        return;
    }
#ifdef TARGET_DC
    if (arg2) {
        shz_xmtrx_load_4x4((const shz_mat4x4_t*)get_projection_matrix_f32());
        shz_xmtrx_apply_rotation_zxy(-obj->trans.rotation.z_rotation / SHZ_FSCA_RAD_FACTOR,
                                     -obj->trans.rotation.x_rotation / SHZ_FSCA_RAD_FACTOR,
                                     -obj->trans.rotation.y_rotation / SHZ_FSCA_RAD_FACTOR);
    } else {
        shz_xmtrx_init_rotation_zxy(-obj->trans.rotation.z_rotation / SHZ_FSCA_RAD_FACTOR,
                                    -obj->trans.rotation.x_rotation / SHZ_FSCA_RAD_FACTOR,
                                    -obj->trans.rotation.y_rotation / SHZ_FSCA_RAD_FACTOR);
    }
    shz_xmtrx_apply_scale(4.0f, 4.0f, 4.0f);

    shz_vec3_deref(&direction) =
        shz_xmtrx_transform_vec3(shz_vec3_init(shading->shadowDirX,
                                               shading->shadowDirY,
                                               shading->shadowDirZ));
#else
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
#endif
    colourBase = shading->unk0 * intensity * 255.0f;
#ifdef TARGET_DC
    ambientFactorF = shading->ambient * colourBase;
    diffuseScale   = (shading->diffuse * colourBase) / (8192.0f * 32768.0f);
#else
    ambientFactor = shading->ambient * colourBase;
    diffuseFactor = shading->diffuse * colourBase;
    dirX = direction.x;
    dirY = direction.y;
    dirZ = direction.z;
#endif
    vertices = obj->curVertData;
    normals = model->normals;
    normIdx = 0;

    for (i = 0; i < model->numberOfBatches; i++) {
        if (model->batches[i].miscData != BATCH_VTX_COL) {
            for (j = model->batches[i].verticesOffset; j < model->batches[i + 1].verticesOffset; j++) {
#ifdef TARGET_DC
                f32 dot = shz_dot6f((f32) normals[normIdx].x, (f32) normals[normIdx].y, (f32) normals[normIdx].z,
                                    direction.x, direction.y, direction.z);
                f32 shadeF;
                if (dot > 0.0f) {
                    shadeF = dot * diffuseScale + ambientFactorF;
                    if (shadeF > 255.0f) {
                        shadeF = 255.0f;
                    }
                } else {
                    shadeF = ambientFactorF;
                }
                vertices[j].r = (u8) shadeF;
                vertices[j].g = (u8) shadeF;
                vertices[j].b = (u8) shadeF;
#else
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
#endif
                vertices[j].a = 255;
                normIdx++;
            }
        } else if (model->batches[i].flags & RENDER_ENVMAP) {
            normIdx += model->batches[i + 1].verticesOffset - model->batches[i].verticesOffset;
        }
    }
}
