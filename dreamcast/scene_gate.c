// The scene gate; see scene_gate.h.
#include "scene_gate.h"

#include <arch/timer.h>
#include <malloc.h>
#include <stdio.h>

extern void pc_retrace_reset(void);
extern void frame_interp_reset(void);
#ifdef DKR_AICAFLOW
extern void dkr_afx_scene_hold(int held);
#endif

extern void pc_texture_load_report(unsigned int sceneMs);
extern void pc_texture_scene_load(int level);
extern void pc_model_scene_release(void);
extern int pc_textures_release_held(void);
extern void pc_model_scene_load(int level);
extern void pc_song_scene_load(int level);
extern void pc_asset_scene_load(int level);
#if DKR_GFX_PROFILE
extern void pc_scene_prof_report(void);
#endif

extern void pc_fsaa_set(int on);

#ifndef DKR_FSAA
#define DKR_FSAA 0
#endif
// The maps drawn without FSAA in an FSAA build (FSAA_OFF_MAPS in Makefile.dc):
// the ones whose translucent fill takes the PVR past a frame with it.
#ifndef DKR_FSAA_OFF_MAPS
#define DKR_FSAA_OFF_MAPS -1
#endif

// Where an FSAA build uses it, from the next scene load: 0 nowhere, 1 on every
// map, 2 on all but FSAA_OFF_MAPS. The debug menu's FSAA row sets it.
int gFsaaMode = DKR_FSAA ? 2 : 0;

static int fsaa_for_level(int level) {
    static const short off[] = { DKR_FSAA_OFF_MAPS, -1 };
    int i;

    if (!DKR_FSAA || gFsaaMode == 0) {
        return 0;
    }
    if (gFsaaMode == 1) {
        return 1;
    }
    for (i = 0; off[i] >= 0; i++) {
        if (off[i] == level) {
            return 0;
        }
    }
    return 1;
}

static volatile int sSceneReady = 1;
static volatile int sAwaitingStep;  // loaded, but its first logic step has not run
static volatile int sAwaitingFrame; // stepped, but its first frame is not on screen yet
static int sOpeningFrame;            // the frame that opened the gate, until the profile asks
static int sSceneLevel = -1;
static unsigned int sSceneVisit;
static unsigned long long sLoadStarted;

void pc_scene_loading(int level) {
#if DKR_GFX_PROFILE
    pc_scene_prof_report(); // the scene that is ending
    {
        // How much of the heap the ending scene left in use, for sizing what
        // the next one may hold.
        struct mallinfo heap = mallinfo();

        printf("SCENE HEAP arena %lu KB, in use %lu KB, free %lu KB\n", (unsigned long) (heap.arena / 1024),
               (unsigned long) (heap.uordblks / 1024), (unsigned long) (heap.fordblks / 1024));
    }
#endif
    sSceneReady = 0;
    sAwaitingStep = sAwaitingFrame = 0;
    sLoadStarted = timer_ms_gettime64();
    sSceneLevel = level;
    sSceneVisit++;
    // The previous scene's held models, sprites and textures go first, so the
    // new level has the room.
    pc_textures_release_held();
    pc_model_scene_release();
    // FSAA for this scene, before anything goes into video memory: switching
    // restarts the PVR and clears it (dreamcast/gfx.c pc_fsaa_set).
    pc_fsaa_set(fsaa_for_level(level));
    // The scene's pack of asset data, in one read, before the level loads.
    pc_asset_scene_load(level);
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

unsigned int pc_scene_visit(void) {
    return sSceneVisit;
}

int pc_scene_is_ready(void) {
    return sSceneReady;
}
