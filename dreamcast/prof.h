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

// What the display list draws next, for the per-scene cost split (SCENE PROF
// in dreamcast/main.c). The game marks it with a tagged no-op that only the
// GFX_PROFILE build emits; the display-list walk times each stretch.
enum {
    PC_PROF_CAT_OTHER,     // camera, sky, weather, transitions, menus
    PC_PROF_CAT_LEVEL,     // opaque level geometry
    PC_PROF_CAT_OBJECTS,   // other objects
    PC_PROF_CAT_LEVEL_TR,  // translucent level geometry
    PC_PROF_CAT_WATER,     // waves
    PC_PROF_CAT_PARTICLES, // translucent objects, particles and effects
    PC_PROF_CAT_HUD,
    PC_PROF_CAT_RACERS,    // racer objects (render_object)
    PC_PROF_CAT_SHADOWS,   // object shadows
    PC_PROF_CAT_COUNT
};
#define PC_PROF_CAT_TAG 0xD0CA7E00u
#if DKR_GFX_PROFILE && defined(TARGET_DC)
#define PC_PROF_CATEGORY(dl, cat) gDPNoOpTag((dl)++, PC_PROF_CAT_TAG | (cat))
#else
#define PC_PROF_CATEGORY(dl, cat) ((void) 0)
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
