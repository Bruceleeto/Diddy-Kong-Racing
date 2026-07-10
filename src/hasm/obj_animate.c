/* Decompiled from src/hasm/ido/obj_animate.s (hand-written asm in retail). */
#include "objects.h"
#include "structs.h"
#include "types.h"

/* Scratch buffer (0xC00 bytes, allocated in object_models.c) holding the
 * current keyframe's interpolated s16 x/y/z delta per animated vertex. */
extern s16 *D_8011D644;

/**
 * Step an object's keyframe vertex animation and write the resulting pose
 * into the model instance's output vertex buffer.
 *
 * Animation data layout (per animation, model->animations[id].animData):
 * - 0x0C-byte keyframe header (s16 fields; 0/1/2 are the model offset,
 *   5 is head tilt), followed by numberOfAnimatedVertices s16 x/y/z triplets:
 *   the base pose deltas added to the model's rest vertices.
 * - Then one record per keyframe: numberOfAnimatedVertices s8 x/y/z delta
 *   triplets, each PRECEDED by the 0x0C-byte header of the keyframe it
 *   leads into. Record k's deltas live at recordSize * (k + 2), where
 *   recordSize = numberOfAnimatedVertices * 3 + 0xC.
 *
 * modelInst->vertices[2] is repurposed as an s16 x/y/z accumulator holding
 * the integer pose at the current keyframe; it persists across calls so the
 * animation can step forward/backward incrementally by applying s8 deltas.
 * The fractional part of the frame (1/16ths) is interpolated separately into
 * D_8011D644 and added on top when composing the output vertices.
 *
 * Returns TRUE if a new pose was written, FALSE if there was nothing to do.
 */
s32 obj_animate(Object *obj) {
    ModelInstance *modelInst;
    ObjectModel *model;
    u8 *animData;
    u8 *delta;
    u8 *header;
    u8 *nextHeader;
    s16 *indices;
    s16 *animState; /* accumulated s16 pose at the current keyframe */
    s16 *fracDelta; /* D_8011D644: fraction-scaled deltas toward the next keyframe */
    Vertex *outVtx;
    s32 modelNum;
    s32 animID;
    s32 frame;       /* current animation position, 12.4 fixed point */
    s32 keyframe;    /* whole keyframe index (frame >> 4) */
    s32 frac;        /* fractional 16ths (frame & 0xF) */
    s32 maxFrame;    /* last valid animation position */
    s32 curKeyframe; /* keyframe currently held in animState (-1 = invalid) */
    s32 recordSize;  /* bytes per keyframe record */
    s32 numAnimVtx;
    s32 val, next;
    s32 slot;
    s32 i;

    modelNum = obj->modelIndex;
    if (modelNum < 0) {
        modelNum = 0;
    }
    if (modelNum >= obj->header->numberOfModelIds) {
        modelNum = obj->header->numberOfModelIds;
    }
    modelInst = obj->modelInstances[modelNum];
    model = modelInst->objModel;
    if (model->animations == NULL) {
        return FALSE;
    }

    frame = obj->animFrame;
    animID = obj->animationID;
    if (frame == modelInst->animationFrameCount && animID == modelInst->animationID) {
        return FALSE;
    }

    if (animID < 0) {
        animID = 0;
    }
    if (animID >= model->numberOfAnimations) {
        animID = model->numberOfAnimations - 1;
    }
    maxFrame = 0;
    if (model->numberOfAnimations > 0) {
        maxFrame = model->animations[animID].animLength - 2;
    }
    keyframe = frame >> 4;
    if (keyframe < 0 || keyframe > maxFrame) {
        /* out of range: restart and force a full pose rebuild */
        modelInst->animationID = -1;
        frame = 0;
        keyframe = 0;
    }

    animState = (s16 *) modelInst->vertices[2];
    if (animID == modelInst->animationID) {
        curKeyframe = modelInst->animationFrame;
    } else {
        curKeyframe = -1;
    }
    modelInst->animationID = animID;
    modelInst->animationFrameCount = frame;
    modelInst->animationFrame = keyframe;
    frac = frame & 0xF;

    animData = model->animations[animID].animData;
    indices = (s16 *) model->animatedVertexIndices;
    numAnimVtx = model->numberOfAnimatedVertices;
    recordSize = numAnimVtx * 3 + 0xC;

    if (keyframe == 0 || curKeyframe == -1) {
        /* rebuild the keyframe-0 pose: rest vertices + base s16 deltas */
        s16 *basePose = (s16 *) (animData + 0xC);
        Vertex *restVtx = model->vertices;

        for (i = 0; i < model->numberOfVertices; i++) {
            slot = indices[i];
            if (slot != -1) {
                animState[slot * 3 + 0] = restVtx->x + basePose[slot * 3 + 0];
                animState[slot * 3 + 1] = restVtx->y + basePose[slot * 3 + 1];
                animState[slot * 3 + 2] = restVtx->z + basePose[slot * 3 + 2];
            }
            restVtx++;
        }
        curKeyframe = 0;
    }

    /* step the accumulated pose forward or backward to the target keyframe */
    if (curKeyframe < keyframe) {
        delta = animData + recordSize * (curKeyframe + 2);
        do {
            s8 *d = (s8 *) delta;
            for (i = 0; i < numAnimVtx; i++) {
                animState[i * 3 + 0] += d[0];
                animState[i * 3 + 1] += d[1];
                animState[i * 3 + 2] += d[2];
                d += 3;
            }
            delta += recordSize;
            curKeyframe++;
        } while (curKeyframe < keyframe);
    }
    if (keyframe < curKeyframe) {
        delta = animData + recordSize * (curKeyframe + 2);
        do {
            s8 *d;
            delta -= recordSize;
            curKeyframe--;
            d = (s8 *) delta;
            for (i = 0; i < numAnimVtx; i++) {
                animState[i * 3 + 0] -= d[0];
                animState[i * 3 + 1] -= d[1];
                animState[i * 3 + 2] -= d[2];
                d += 3;
            }
        } while (keyframe < curKeyframe);
    }

    /* scale the next keyframe's deltas by the frame fraction */
    delta = animData + recordSize * (curKeyframe + 2);
    fracDelta = D_8011D644;
    for (i = 0; i < numAnimVtx; i++) {
        fracDelta[i * 3 + 0] = (((s8 *) delta)[0] * frac) >> 4;
        fracDelta[i * 3 + 1] = (((s8 *) delta)[1] * frac) >> 4;
        fracDelta[i * 3 + 2] = (((s8 *) delta)[2] * frac) >> 4;
        delta += 3;
    }

    /* interpolate the keyframe header (model offset + head tilt) between the
     * current and next keyframe; the header values are stored bytewise
     * (s8 high byte, u8 low byte) */
#define HEADER_S16(p, ofs) ((((s8 *) (p))[ofs] << 8) | (p)[(ofs) + 1])
    if (keyframe == 0) {
        header = animData;
        nextHeader = header + (recordSize * 2) - 0xC;
    } else {
        header = animData + recordSize * (keyframe + 1) - 0xC;
        nextHeader = header + recordSize;
    }
    val = HEADER_S16(header, 0x0);
    next = HEADER_S16(nextHeader, 0x0);
    modelInst->offsetX = val + (((next - val) * frac) >> 4);
    val = HEADER_S16(header, 0x2);
    next = HEADER_S16(nextHeader, 0x2);
    modelInst->offsetY = val + (((next - val) * frac) >> 4);
    val = HEADER_S16(header, 0x4);
    next = HEADER_S16(nextHeader, 0x4);
    modelInst->offsetZ = val + (((next - val) * frac) >> 4);
    val = HEADER_S16(header, 0xA);
    next = HEADER_S16(nextHeader, 0xA);
    modelInst->headTilt = val + (((next - val) * frac) >> 4);
#undef HEADER_S16

    /* compose base pose + fractional deltas into the other task buffer */
    modelInst->animationTaskNum ^= 1;
    outVtx = modelInst->vertices[(s32) modelInst->animationTaskNum];
    fracDelta = D_8011D644;
    for (i = 0; i < model->numberOfVertices; i++) {
        slot = indices[i];
        if (slot != -1) {
            outVtx->x = animState[slot * 3 + 0] + fracDelta[slot * 3 + 0];
            outVtx->y = animState[slot * 3 + 1] + fracDelta[slot * 3 + 1];
            outVtx->z = animState[slot * 3 + 2] + fracDelta[slot * 3 + 2];
        }
        outVtx++;
    }
    return TRUE;
}
