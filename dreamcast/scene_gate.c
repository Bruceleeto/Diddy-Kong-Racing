// The scene gate; see scene_gate.h.
#include "scene_gate.h"

#include <arch/timer.h>

extern void pc_retrace_reset(void);
extern void frame_interp_reset(void);
#ifdef DKR_AICAFLOW
extern void dkr_afx_scene_hold(int held);
#endif

extern void pc_texture_load_report(unsigned int sceneMs);
extern void pc_texture_scene_load(int level);
extern void pc_model_scene_release(void);
extern void pc_model_scene_load(int level);
extern void pc_song_scene_load(int level);
#if DKR_GFX_PROFILE
extern void pc_scene_prof_report(void);
#endif

static volatile int sSceneReady = 1;
static volatile int sAwaitingStep;  // loaded, but its first logic step has not run
static volatile int sAwaitingFrame; // stepped, but its first frame is not on screen yet
static int sOpeningFrame;            // the frame that opened the gate, until the profile asks
static int sSceneLevel = -1;
static unsigned long long sLoadStarted;

void pc_scene_loading(int level) {
#if DKR_GFX_PROFILE
    pc_scene_prof_report(); // the scene that is ending
#endif
    sSceneReady = 0;
    sAwaitingStep = sAwaitingFrame = 0;
    sLoadStarted = timer_ms_gettime64();
    sSceneLevel = level;
    // The previous scene's held models go first, so the new level has the room.
    pc_model_scene_release();
    // The scene's textures first, in one pass, before the game asks for any.
    pc_texture_scene_load(level);
#ifdef DKR_AICAFLOW
    dkr_afx_scene_hold(1);
#endif
}

void pc_scene_ready(void) {
    // Models the scene's objects load later (the character demo swaps racers),
    // loaded now and held until the next scene loads.
    pc_model_scene_load(sSceneLevel);
    // And the song a cutscene starts a few frames in.
    pc_song_scene_load(sSceneLevel);
    // The scene's first logic step still belongs to the load: it spawns the
    // objects, and a cutscene's animation objects start their song there.
    sAwaitingStep = 1;
}

void pc_scene_step_done(void) {
    if (!sAwaitingStep) {
        return;
    }
    sAwaitingStep = 0;
    pc_texture_load_report((unsigned int) (timer_ms_gettime64() - sLoadStarted));
    // The load is over: the frame pacer and the interpolation start afresh, so
    // the load's time is not played back as game time.
    pc_retrace_reset();
    frame_interp_reset();
    // Sound waits until the scene can be seen: the game may still skip drawing
    // its first frames, and a sound should not start over the old picture.
    sAwaitingFrame = 1;
}

void pc_scene_frame_shown(void) {
    // A scene that draws without stepping (no objects) ends its load here.
    pc_scene_step_done();
    if (!sAwaitingFrame) {
        return;
    }
    sAwaitingFrame = 0;
#ifdef DKR_AICAFLOW
    dkr_afx_scene_hold(0);
#endif
    sSceneReady = 1;
    sOpeningFrame = 1;
}

int pc_scene_take_opening_frame(void) {
    int opening = sOpeningFrame;

    sOpeningFrame = 0;
    return opening;
}

int pc_scene_level(void) {
    return sSceneLevel;
}

int pc_scene_is_ready(void) {
    return sSceneReady;
}
