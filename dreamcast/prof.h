#ifndef DKR_DREAMCAST_PROF_H
#define DKR_DREAMCAST_PROF_H

// Times parts of the game's own frame for the GFX_PROFILE build; see the
// frame profile in dreamcast/main.c. Costs nothing in a normal build.
enum {
    PC_PROF_LOGIC,    // obj_update: racers, AI, objects, collision
    PC_PROF_SCENE,    // render_scene as a whole, including the parts below
    PC_PROF_WAVES,
    PC_PROF_SHADOWS,
    PC_PROF_VIEWPORT, // track and object display lists for one viewport
    PC_PROF_HUD,
    PC_PROF_COUNT
};

#ifndef DKR_GFX_PROFILE
#define DKR_GFX_PROFILE 0
#endif
#if DKR_GFX_PROFILE
extern unsigned long long pc_uptime_ns(void);
void pc_prof_add(int part, unsigned long long ns);
#define PC_PROF(part, call) \
    do { \
        unsigned long long pcProfStart = pc_uptime_ns(); \
        call; \
        pc_prof_add(part, pc_uptime_ns() - pcProfStart); \
    } while (0)
#else
#define PC_PROF(part, call) call
#endif

#endif
