// Fixed-step logic under a faster display.
//
// The game's physics is tuned for one logic step per two video fields (30 Hz on
// NTSC). Much of it applies a change once per step with no regard for how long
// the step was, so stepping it once per field makes every racer accelerate, brake
// and boost about twice as hard. Rather than rescale each such line, the logic
// keeps its two-field step and the display runs ahead of it: a frame that steps
// the logic draws the scene part-way between the old state and the new one, and
// the following frame draws the new state without stepping. Behaviour is then
// exactly what it is at 30 Hz, and motion is still drawn at 60.
//
// At 30 Hz or slower every frame steps and draws the new state, as before.
#include <ultra64.h>
#include <structs.h>
#include "camera.h"
#include "objects.h"

#define INTERP_MAX_OBJECTS 1024
// A larger move in one step is a respawn, warp or camera cut, not motion.
#define INTERP_MAX_MOVE 400.0f

extern Object **gObjPtrList;
extern s32 gObjectCount;
extern Camera gCameras[8];
extern u16 gControllerButtonsPressed[];
extern u16 gControllerButtonsReleased[];

typedef struct {
    Object *obj;
    ObjectTransform trans;
} InterpObject;

static InterpObject sBefore[INTERP_MAX_OBJECTS]; // where each object was before the step
// The game re-sorts its object list by distance every frame, so an object is
// found again by address, not by its place in the list.
#define INTERP_HASH 4096
static u16 sBeforeAt[INTERP_HASH]; // index into sBefore plus one; 0 is empty

static u32 interp_hash(const Object *obj) {
    return (((u32) obj >> 4) ^ ((u32) obj >> 13)) & (INTERP_HASH - 1);
}

static s32 interp_find(const Object *obj) {
    u32 at = interp_hash(obj);

    while (sBeforeAt[at] != 0) {
        if (sBefore[sBeforeAt[at] - 1].obj == obj) {
            return sBeforeAt[at] - 1;
        }
        at = (at + 1) & (INTERP_HASH - 1);
    }
    return -1;
}
static InterpObject sAfter[INTERP_MAX_OBJECTS];  // the stepped transforms the draw displaced
static s32 sBeforeCount, sAfterCount;
static ObjectTransform sCamBefore[8], sCamAfter[8];
static s32 sAhead;       // fields of game time already simulated but not yet shown
static f32 sAlpha = 1.0f; // how far from the old state to the new this frame draws
static s32 sStepped;     // the logic stepped this frame, so there is an old state to draw from
static s32 sDisplaced;
static u16 sCarryPressed[4], sCarryReleased[4];

/**
 * How many fields the logic should advance this frame, given how many went by.
 * Zero means the previous step already covers this frame.
 */
s32 frame_interp_step(s32 fields) {
    s32 step;

    sStepped = FALSE;
    if (sAhead >= fields) {
        sAhead -= fields;
        sAlpha = 1.0f;
        return 0;
    }
    step = fields - sAhead;
    if (step < 2) {
        step = 2;
    }
    sAhead += step - fields;
    sAlpha = (f32) (step - sAhead) / (f32) step;
    return step;
}

/** Call before a logic step: remembers where everything is. */
void frame_interp_before_step(void) {
    s32 i;

    sBeforeCount = (gObjectCount < INTERP_MAX_OBJECTS) ? gObjectCount : INTERP_MAX_OBJECTS;
    for (i = 0; i < INTERP_HASH; i++) {
        sBeforeAt[i] = 0;
    }
    for (i = 0; i < sBeforeCount; i++) {
        u32 at = interp_hash(gObjPtrList[i]);

        sBefore[i].obj = gObjPtrList[i];
        sBefore[i].trans = gObjPtrList[i]->trans;
        while (sBeforeAt[at] != 0) {
            at = (at + 1) & (INTERP_HASH - 1);
        }
        sBeforeAt[at] = (u16) (i + 1);
    }
    for (i = 0; i < 8; i++) {
        sCamBefore[i] = gCameras[i].trans;
    }
    // A button edge lasts one frame. The logic must also see the ones that came
    // and went on frames it did not step.
    for (i = 0; i < 4; i++) {
        gControllerButtonsPressed[i] |= sCarryPressed[i];
        gControllerButtonsReleased[i] |= sCarryReleased[i];
        sCarryPressed[i] = sCarryReleased[i] = 0;
    }
    sStepped = TRUE;
}

/** Call on a frame the logic does not step. */
void frame_interp_skipped(void) {
    s32 i;

    for (i = 0; i < 4; i++) {
        sCarryPressed[i] |= gControllerButtonsPressed[i];
        sCarryReleased[i] |= gControllerButtonsReleased[i];
    }
}

static s32 interp_transform(ObjectTransform *now, const ObjectTransform *before, f32 alpha) {
    f32 dx = now->x_position - before->x_position;
    f32 dy = now->y_position - before->y_position;
    f32 dz = now->z_position - before->z_position;

    if ((dx * dx) + (dy * dy) + (dz * dz) > INTERP_MAX_MOVE * INTERP_MAX_MOVE) {
        return FALSE;
    }
    now->x_position = before->x_position + (dx * alpha);
    now->y_position = before->y_position + (dy * alpha);
    now->z_position = before->z_position + (dz * alpha);
    now->scale = before->scale + ((now->scale - before->scale) * alpha);
    // Angles wrap, so go the short way round.
    now->rotation.y_rotation = before->rotation.y_rotation +
                               (s16) ((s16) (now->rotation.y_rotation - before->rotation.y_rotation) * alpha);
    now->rotation.x_rotation = before->rotation.x_rotation +
                               (s16) ((s16) (now->rotation.x_rotation - before->rotation.x_rotation) * alpha);
    now->rotation.z_rotation = before->rotation.z_rotation +
                               (s16) ((s16) (now->rotation.z_rotation - before->rotation.z_rotation) * alpha);
    return TRUE;
}

/** Call before drawing the scene: moves everything to where this frame shows it. */
void frame_interp_draw_begin(void) {
    s32 i;

    sDisplaced = FALSE;
    sAfterCount = 0;
    if (!sStepped || sAlpha >= 1.0f) {
        return;
    }
    sDisplaced = TRUE;
    for (i = 0; i < gObjectCount && sAfterCount < INTERP_MAX_OBJECTS; i++) {
        Object *obj = gObjPtrList[i];
        s32 j;

        j = interp_find(obj);
        if (j < 0) {
            continue; // spawned by this step: it has no earlier position
        }
        sAfter[sAfterCount].obj = obj;
        sAfter[sAfterCount].trans = obj->trans;
        if (interp_transform(&obj->trans, &sBefore[j].trans, sAlpha)) {
            sAfterCount++;
        }
    }
    for (i = 0; i < 8; i++) {
        sCamAfter[i] = gCameras[i].trans;
        (void) interp_transform(&gCameras[i].trans, &sCamBefore[i], sAlpha);
    }
}

/** Call after drawing the scene: puts the stepped state back. */
void frame_interp_draw_end(void) {
    s32 i;

    if (!sDisplaced) {
        return;
    }
    for (i = 0; i < sAfterCount; i++) {
        sAfter[i].obj->trans = sAfter[i].trans;
    }
    for (i = 0; i < 8; i++) {
        gCameras[i].trans = sCamAfter[i];
    }
    sDisplaced = FALSE;
    sStepped = FALSE;
}

/** A scene change: nothing carries over. */
void frame_interp_reset(void) {
    sAhead = 0;
    sAlpha = 1.0f;
    sStepped = FALSE;
    sBeforeCount = 0;
}
