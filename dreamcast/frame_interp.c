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

#include <math.h>
#include <sh4zam/shz_sh4zam.h>

#define INTERP_MAX_OBJECTS 512 // OBJECT_SLOT_COUNT in src/objects.c
// A larger move in one step is a respawn, warp or camera cut, not motion.
#define INTERP_MAX_MOVE 400.0f

extern Object **gObjPtrList;
extern s32 gObjectCount;
extern Camera gCameras[8];
extern u16 gControllerButtonsPressed[];
extern u16 gControllerButtonsReleased[];
extern s8 gCutsceneCameraActive;

typedef struct {
    Object *obj;
    ObjectTransform trans;
} InterpObject;

static InterpObject sBefore[INTERP_MAX_OBJECTS]; // where each object was before the step
// The game re-sorts its object list by distance every frame, so an object is
// found again by address, not by its place in the list.
#define INTERP_HASH 2048
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
static s8 sCutsceneCamera; // whether the last step left the cutscene camera on

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

/** Call after a logic step. */
void frame_interp_after_step(void) {
    sCutsceneCamera = gCutsceneCameraActive;
}

/** Call on a frame the logic does not step. */
void frame_interp_skipped(void) {
    s32 i;

    // The main loop turns the cutscene camera off every frame and the cutscene's
    // own update turns it back on. Without an update this frame it has to be
    // carried over, or the scene is drawn through the player's camera instead.
    gCutsceneCameraActive = sCutsceneCamera;

    for (i = 0; i < 4; i++) {
        sCarryPressed[i] |= gControllerButtonsPressed[i];
        sCarryReleased[i] |= gControllerButtonsReleased[i];
    }
}

#define INTERP_RAD_TO_ANGLE (32768.0f / SHZ_F_PI) // radians to 16-bit binary angle units

/**
 * The rotation part-way between two. Angles wrap, so each axis goes the short
 * way round. When only one axis turns that is exact; when several do, stepping
 * each angle on its own is not the rotation in between, so the two orientations
 * are slerped and the angles taken back out in the order the transform applies
 * them: Y, then X, then Z (xmtrx_init_transform, shz_xmtrx_init_rotation_yxz).
 */
static void interp_rotation(Vec3s *now, const Vec3s *before, f32 alpha) {
    s16 dy = (s16) (now->y_rotation - before->y_rotation);
    s16 dx = (s16) (now->x_rotation - before->x_rotation);
    s16 dz = (s16) (now->z_rotation - before->z_rotation);
    shz_quat_t q;
    f32 sinX;

    if ((dy != 0) + (dx != 0) + (dz != 0) < 2) {
        now->y_rotation = before->y_rotation + (s16) (dy * alpha);
        now->x_rotation = before->x_rotation + (s16) (dx * alpha);
        now->z_rotation = before->z_rotation + (s16) (dz * alpha);
        return;
    }
    q = shz_quat_slerp(shz_quat_from_rotation_yxz_u16(before->y_rotation, before->x_rotation, before->z_rotation),
                       shz_quat_from_rotation_yxz_u16(now->y_rotation, now->x_rotation, now->z_rotation), alpha);
    sinX = shz_clampf(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);
    now->x_rotation = (s16) (s32) (asinf(sinX) * INTERP_RAD_TO_ANGLE);
    now->y_rotation = (s16) (s32) (atan2f(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) *
                             INTERP_RAD_TO_ANGLE);
    now->z_rotation = (s16) (s32) (atan2f(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z)) *
                             INTERP_RAD_TO_ANGLE);
}

static s32 interp_transform(ObjectTransform *now, const ObjectTransform *before, f32 alpha) {
    shz_vec3_t from = shz_vec3_init(before->x_position, before->y_position, before->z_position);
    shz_vec3_t to = shz_vec3_init(now->x_position, now->y_position, now->z_position);

    if (shz_vec3_distance_sqr(from, to) > INTERP_MAX_MOVE * INTERP_MAX_MOVE) {
        return FALSE;
    }
    shz_vec3_t pos = shz_vec3_lerp(from, to, alpha);
    now->x_position = pos.x;
    now->y_position = pos.y;
    now->z_position = pos.z;
    now->scale = shz_lerpf(before->scale, now->scale, alpha);
    interp_rotation(&now->rotation, &before->rotation, alpha);
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

/** The display rate changed: the next step starts from the state on screen. */
void frame_interp_realign(void) {
    sAhead = 0;
}

/** A scene change: nothing carries over. */
void frame_interp_reset(void) {
    sCutsceneCamera = FALSE;
    sAhead = 0;
    sAlpha = 1.0f;
    sStepped = FALSE;
    sBeforeCount = 0;
}
