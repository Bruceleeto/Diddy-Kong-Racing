// The scene gate; see scene_gate.h.
#include "scene_gate.h"

extern void pc_retrace_reset(void);
extern void frame_interp_reset(void);
#ifdef DKR_AICAFLOW
extern void dkr_afx_scene_hold(int held);
#endif

static volatile int sSceneReady = 1;

void pc_scene_loading(void) {
    sSceneReady = 0;
#ifdef DKR_AICAFLOW
    dkr_afx_scene_hold(1);
#endif
}

void pc_scene_ready(void) {
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
