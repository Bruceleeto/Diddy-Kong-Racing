// The scene gate; see scene_gate.h.
#include "scene_gate.h"

#include <arch/timer.h>

extern void pc_retrace_reset(void);
extern void frame_interp_reset(void);
#ifdef DKR_AICAFLOW
extern void dkr_afx_scene_hold(int held);
#endif

extern void pc_texture_load_report(unsigned int sceneMs);

static volatile int sSceneReady = 1;
static unsigned long long sLoadStarted;

void pc_scene_loading(void) {
    sSceneReady = 0;
    sLoadStarted = timer_ms_gettime64();
#ifdef DKR_AICAFLOW
    dkr_afx_scene_hold(1);
#endif
}

void pc_scene_ready(void) {
    pc_texture_load_report((unsigned int) (timer_ms_gettime64() - sLoadStarted));
    // The load is over: the frame pacer and the interpolation start afresh, so
    // the load's time is not played back as game time.
    pc_retrace_reset();
    frame_interp_reset();
#ifdef DKR_AICAFLOW
    dkr_afx_scene_hold(0);
#endif
    sSceneReady = 1;
}

int pc_scene_is_ready(void) {
    return sSceneReady;
}
