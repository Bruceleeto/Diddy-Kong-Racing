#include "dkr_asset_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>
#include <time.h>
#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "gfx.h"
#include <enDjinn/enj_texture.h>


extern u64 pc_uptime_ns(void);
#include "prof.h"
#include "scene_gate.h"
#include "scrawl.h"
#include "swirl.h"
#include "cover.h"

#define DKR_TEXTURE_PACK DKR_ASSET_MOUNT "/build/dc/textures.dtp"
#ifndef DKR_TEX_TRACE
#define DKR_TEX_TRACE 0 // log why textures are decoded instead of prebuilt
#endif
static u32 sTexDecodes, sTexPrebuiltHits; // what texture lookups did, for the profile
static u32 sTexLoadsInFrame;  // prebuilt textures read since the last frame
static u32 sModelLoadsInFrame; // object models read from disc since the last frame
static u64 sTexLoadNsInFrame;

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread.
void thread3_main(void *unused);

extern OSThread *__osRunningThread;
static OSThread sHostThread;

#define PC_RETRACE_NSEC (1000000000ll / 60)
#ifndef PC_LOGIC_UPDATE_RATE
#define PC_LOGIC_UPDATE_RATE 2 // LOGIC_30FPS — what gVideoDeltaTime commits to
#endif
#define PC_MAX_UPDATE_RATE 6   // don't let a debugger pause become a huge skip

// ---------------------------------------------------------------------------
// Frame profile (GFX_PROFILE=1): where a frame's time goes and how much
// geometry it submits. GFX_PROFILE_SKIP=1 transforms and culls but sends no
// vertices; =2 only resolves each batch's state; =3 skips batches that take the
// emulated path. These draw incomplete frames and exist to split the
// display-list time by subtraction.
// ---------------------------------------------------------------------------
#ifndef DKR_GFX_PROFILE
#define DKR_GFX_PROFILE 0
#endif
#ifndef DKR_GFX_PROFILE_SKIP
#define DKR_GFX_PROFILE_SKIP 0
#endif
#ifndef DKR_AUTO_EXIT_SECONDS
#define DKR_AUTO_EXIT_SECONDS 0
#endif
#if DKR_GFX_PROFILE
// The profile's lines wait in RAM and go out while a scene loads (or at exit).
// A printf over dcload is a round trip to the host: printed mid-scene, the
// lines about a long frame made the next frames long too.
#include <stdarg.h>
static char sProfLog[64 * 1024];
static u32 sProfLogLength, sProfLogDropped;

void pc_prof_vlog(const char *format, va_list args) {
    u32 room = sizeof(sProfLog) - sProfLogLength;
    int n = vsnprintf(sProfLog + sProfLogLength, room, format, args);

    if (n < 0 || (u32) n >= room) {
        sProfLog[sProfLogLength] = '\0';
        sProfLogDropped++;
        return;
    }
    sProfLogLength += (u32) n;
}

void pc_prof_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void pc_prof_log(const char *format, ...) {
    va_list args;

    va_start(args, format);
    pc_prof_vlog(format, args);
    va_end(args);
}

/** Prints what the profile logged; between scenes, where it costs no frame. */
void pc_prof_log_flush(void) {
    if (sProfLogLength > 0) {
        fwrite(sProfLog, 1, sProfLogLength, stdout);
        fflush(stdout);
    }
    if (sProfLogDropped > 0) {
        printf("PROF LOG full: %lu lines dropped\n", (unsigned long) sProfLogDropped);
    }
    sProfLogLength = sProfLogDropped = 0;
}

enum { PROF_FRAMES = 60 };
static struct {
    u64 last, wait, waitFrame, begin, dl, end;
    u32 frames, blast, fallback, verts, tris, culled, clipped, sent, slowTris, rects, sentFrame, maxSent;
    u32 slowBatches;
    u32 trStats[10]; // see gfx_tr_stats
    u32 noDepth[4];  // see gfx_nodepth_stats
    u64 part[PC_PROF_COUNT]; // the game's own frame, see prof.h
    u32 workMax; // the longest frame, start to start, less the pacer's sleep
    u32 missed;  // frames that took more video fields than the target
    // Why a batch missed the fast path, in triangles.
    u32 whyCombiner, whyBillboard, whyState, whyFull, whyAppend, whyShared, whyLoose;
    u32 whyStateKind[6]; // no scene, texenv blend, alpha test, no depth test, depth offset, scissor
    u64 pvrRender, pvrRegister;
    u32 pvrBytes, ptWords, trWords;
    u64 tBlast, tFallback, tVtx, tPoly, tRect, tMtx;
    u64 tState, tXform, tEmit, tPolyState, tPolyEmit, tPolyDraw;
} sProf;
#define PROF_ADD(field, n) (sProf.field += (n))

// ---------------------------------------------------------------------------
// Per-scene profile (SCENE PROF), printed as the next scene starts loading:
// - late frames counted on the PVR's vblank counter, not the frame's own time;
// - how long the pacer overslept its target;
// - display-list time and triangles by category, as the game marks them
//   (PC_PROF_CATEGORY in prof.h);
// - how much level geometry a per-batch frustum test would drop (src/tracks.c).
// ---------------------------------------------------------------------------
static const char *const sCatNames[PC_PROF_CAT_COUNT] = {
    "other", "level", "objects", "level-tr", "water", "particles", "hud", "racers", "shadows",
};
static struct {
    u32 frames, lateFrames, lateFields, maxFields;
    u64 overNs, maxOverNs;
    u64 catNs[PC_PROF_CAT_COUNT];
    u32 catTris[PC_PROF_CAT_COUNT];
    u32 levelBatches, levelTris, cullBatches, cullTris;
    u32 cullMatrixChanged; // frames where objects changed gViewProjMatrixF before the translucent level
    u32 falseCulls;        // culled batches with a vertex in view
    u32 levelVtx, levelVtxOtherMatrix; // level G_VTX loads, and those not drawn with the cull matrix
    // CPU cycles the display-list walk stood still on operand-cache and on
    // instruction-cache misses, each counted on every other frame.
    u64 walkStall[2];
} sScene;
static u32 sProfWalkSp; // where the walk's stack is, for the cache layout
static const float (*sLevelCullMatrix)[4];

// Vertices that ran the combiner per vertex (not passthrough or constant), by
// combiner, for the scene's report.
#define PROF_SLOW_CC 16
static struct {
    u32 w0, w1, cyc, prim, env, verts;
} sSlowCc[PROF_SLOW_CC];

static void prof_slow_cc(u32 verts);
static u32 sCcClassified, sCcProbed; // combiner_classify calls, and those that ran the probes

// What the display-list walk spends on racers (PC_PROF_CAT_RACERS), by
// handler: the timers need GFX_PROFILE=2, the counts work at 1 too.
#define RACER_PROF_FIELDS(X)                                                                                         \
    X(blast) X(fallback) X(verts) X(tris) X(culled) X(clipped) X(sent) X(slowTris) X(rects) X(slowBatches)           \
    X(whyCombiner) X(whyBillboard) X(whyState) X(whyFull) X(whyAppend) X(whyShared) X(whyLoose) X(tBlast)           \
    X(tFallback) X(tVtx) X(tPoly) X(tRect) X(tMtx) X(tState) X(tXform) X(tEmit) X(tPolyState) X(tPolyEmit)          \
    X(tPolyDraw)
#define RACER_PROF_DECLARE(f) u64 f;
static struct { RACER_PROF_FIELDS(RACER_PROF_DECLARE) } sRacerProf, sRacerStart;
#define RACER_PROF_SNAP(f) sRacerStart.f = sProf.f;
#define RACER_PROF_ADD(f) sRacerProf.f += sProf.f - sRacerStart.f;
#define RACER_PROF_PRINT(f) printf(" %s=%llu", #f, (unsigned long long) (sRacerProf.f / n));
static s32 sCatCur;
static u64 sCatStart;
static u32 sCatTrisStart;
static u32 sSceneVbl; // vblank count at the previous frame's start; 0 after a load

/** Closes the current display-list stretch and starts one of `cat`. */
static void scene_category(s32 cat) {
    u64 now = pc_uptime_ns();

    if (sCatCur == PC_PROF_CAT_RACERS) {
        RACER_PROF_FIELDS(RACER_PROF_ADD)
    }
    if (cat == PC_PROF_CAT_RACERS) {
        RACER_PROF_FIELDS(RACER_PROF_SNAP)
    }

    sScene.catNs[sCatCur] += now - sCatStart;
    sScene.catTris[sCatCur] += sProf.tris - sCatTrisStart;
    sCatCur = (cat >= 0 && cat < PC_PROF_CAT_COUNT) ? cat : PC_PROF_CAT_OTHER;
    sCatStart = now;
    sCatTrisStart = sProf.tris;
}

void pc_prof_cull_matrix_changed(void) {
    sScene.cullMatrixChanged++;
}

void pc_prof_cull_matrix(const float m[4][4]) {
    sLevelCullMatrix = m;
}

void pc_prof_false_cull(void) {
    sScene.falseCulls++;
}

/** src/tracks.c: one level batch drawn, and whether a frustum test would drop it. */
void pc_prof_level_batch(s32 tris, s32 culled) {
    sScene.levelBatches++;
    sScene.levelTris += tris;
    if (culled) {
        sScene.cullBatches++;
        sScene.cullTris += tris;
    }
}

/** Prints the scene that just ended and starts counting the next one. */
void pc_scene_prof_report(void) {
    u32 n = sScene.frames ? sScene.frames : 1;
    s32 c;

    pc_prof_log_flush();

    if (sScene.frames > 0) {
        printf("SCENE PROF %lu frames: late %lu (%lu fields, max %lu), pacer oversleep %lu us max %lu us | "
               "level %lu batches %lu tris/frame, frustum would drop %lu batches %lu tris (%lu%%) |",
               (unsigned long) sScene.frames, (unsigned long) sScene.lateFrames, (unsigned long) sScene.lateFields,
               (unsigned long) sScene.maxFields, (unsigned long) (sScene.overNs / n / 1000u),
               (unsigned long) (sScene.maxOverNs / 1000u), (unsigned long) (sScene.levelBatches / n),
               (unsigned long) (sScene.levelTris / n), (unsigned long) (sScene.cullBatches / n),
               (unsigned long) (sScene.cullTris / n),
               (unsigned long) (sScene.levelTris ? sScene.cullTris * 100u / sScene.levelTris : 0));
        for (c = 0; c < PC_PROF_CAT_COUNT; c++) {
            printf(" %s %lu us %lu tris", sCatNames[c], (unsigned long) (sScene.catNs[c] / n / 1000u),
                   (unsigned long) (sScene.catTris[c] / n));
        }
        printf(" | walk stall data %lu us code %lu us", (unsigned long) (sScene.walkStall[0] * 2u / n / 200u),
               (unsigned long) (sScene.walkStall[1] * 2u / n / 200u));
        printf(" | walk stack %08lx", (unsigned long) sProfWalkSp);
        printf("\nSCENE CC classify %lu/frame, probed %lu/frame", (unsigned long) (sCcClassified / n),
               (unsigned long) (sCcProbed / n));
        sCcClassified = sCcProbed = 0;
        for (c = 0; c < PROF_SLOW_CC; c++) {
            if (sSlowCc[c].verts != 0) {
                printf("\nSCENE SLOWCC %lu verts/frame cc=%08lx/%08lx cyc=%lu prim=%08lx env=%08lx",
                       (unsigned long) (sSlowCc[c].verts / n), (unsigned long) sSlowCc[c].w0,
                       (unsigned long) sSlowCc[c].w1, (unsigned long) (sSlowCc[c].cyc >> G_MDSFT_CYCLETYPE),
                       (unsigned long) sSlowCc[c].prim, (unsigned long) sSlowCc[c].env);
            }
            sSlowCc[c].verts = 0;
        }
        printf(" | view matrix changed under the level in %lu frames, %lu false culls, %lu of %lu level vertex loads "
               "not drawn with the cull matrix\n", (unsigned long) sScene.cullMatrixChanged,
               (unsigned long) sScene.falseCulls, (unsigned long) sScene.levelVtxOtherMatrix,
               (unsigned long) sScene.levelVtx);
        {
            extern void pc_asset_play_report(void);
            pc_asset_play_report();
        }
        if (sRacerProf.tris > 0) {
            printf("RACER PROF per frame (ns for t*):");
            RACER_PROF_FIELDS(RACER_PROF_PRINT)
            printf("\n");
        }
    }
    memset(&sScene, 0, sizeof(sScene));
    memset(&sRacerProf, 0, sizeof(sRacerProf));
    sSceneVbl = 0;
}
void pc_prof_add(int part, unsigned long long ns) {
    sProf.part[part] += ns;
}
static s32 sProfVtxWhy; // how the vertices an unfused G_TRIN draws were loaded
// Time of one handler call. A fallback is inside tBlast as well. These read
// the clock a few hundred times a frame, which costs a millisecond or two, so
// they need GFX_PROFILE=2; the frame totals stay honest at 1 and 3.
#if DKR_GFX_PROFILE == 2
#define PROF_TIME(field, call) \
    do { \
        u64 profStart = pc_uptime_ns(); \
        call; \
        sProf.field += pc_uptime_ns() - profStart; \
    } while (0)
// Splits one function into consecutive parts.
#define PROF_LAP_START() u64 profLap = pc_uptime_ns()
#define PROF_LAP(field) \
    do { \
        u64 profNow = pc_uptime_ns(); \
        sProf.field += profNow - profLap; \
        profLap = profNow; \
    } while (0)
#else
#define PROF_TIME(field, call) call
#define PROF_LAP_START() ((void) 0)
#define PROF_LAP(field) ((void) 0)
#endif

// What the blended batches are: triangles by render mode, combiner, texture
// kind and where their alpha comes from. Printed with the frame profile.
#define PROF_BLEND_KINDS 48
static struct {
    u32 modeL, cc0, cc1;
    u8 texKind, vtxAlphaFull, outAlphaFull, fog, primA, envA, used;
    u32 tris;
} sProfBlend[PROF_BLEND_KINDS];

static void prof_blend(u32 modeL, u32 cc0, u32 cc1, s32 texKind, s32 vtxAlphaFull, s32 outAlphaFull, s32 fog,
                       s32 primA, s32 envA, s32 tris) {
    s32 i;

    for (i = 0; i < PROF_BLEND_KINDS; i++) {
        if (!sProfBlend[i].used) {
            sProfBlend[i].modeL = modeL;
            sProfBlend[i].cc0 = cc0;
            sProfBlend[i].cc1 = cc1;
            sProfBlend[i].texKind = (u8) texKind;
            sProfBlend[i].vtxAlphaFull = (u8) vtxAlphaFull;
            sProfBlend[i].outAlphaFull = (u8) outAlphaFull;
            sProfBlend[i].fog = (u8) fog;
            sProfBlend[i].primA = (u8) primA;
            sProfBlend[i].envA = (u8) envA;
            sProfBlend[i].used = 1;
        }
        if (sProfBlend[i].modeL == modeL && sProfBlend[i].cc0 == cc0 && sProfBlend[i].cc1 == cc1 &&
            sProfBlend[i].texKind == texKind && sProfBlend[i].vtxAlphaFull == vtxAlphaFull &&
            sProfBlend[i].outAlphaFull == outAlphaFull && sProfBlend[i].fog == fog && sProfBlend[i].primA == primA &&
            sProfBlend[i].envA == envA) {
            sProfBlend[i].tris += (u32) tris;
            return;
        }
    }
}

static void prof_blend_print(u32 frames) {
    s32 n, i;

    for (n = 0; n < 8; n++) {
        s32 best = -1;

        for (i = 0; i < PROF_BLEND_KINDS; i++) {
            if (sProfBlend[i].used && sProfBlend[i].tris && (best < 0 || sProfBlend[i].tris > sProfBlend[best].tris)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        pc_prof_log("GFX BLEND: tris=%lu modeL=%08lx cc=%08lx/%08lx tex=%d vtxA255=%d outA255=%d fog=%d primA=%d envA=%d\n",
               (unsigned long) (sProfBlend[best].tris / frames), (unsigned long) sProfBlend[best].modeL,
               (unsigned long) sProfBlend[best].cc0, (unsigned long) sProfBlend[best].cc1, sProfBlend[best].texKind,
               sProfBlend[best].vtxAlphaFull, sProfBlend[best].outAlphaFull, sProfBlend[best].fog,
               sProfBlend[best].primA, sProfBlend[best].envA);
        sProfBlend[best].tris = 0;
    }
    memset(sProfBlend, 0, sizeof(sProfBlend));
}

#define PROF_SINCE(now, last) ((now) >= (last) ? (now) - (last) : (now))
static void prof_frame(u64 t0, u64 t1, u64 t2, u64 t3) {
    sProf.begin += t1 - t0;
    sProf.dl += t2 - t1;
    sProf.end += t3 - t2;
    {
        static u64 sFrameStart;
        u32 work = sFrameStart ? (u32) ((t3 - sFrameStart - sProf.waitFrame) / 1000u) : 0;

        if (work > sProf.workMax) sProf.workMax = work;
        // What each frame was made of, so a long one can say why.
        static u64 sLastPart[PC_PROF_COUNT], sLastAfxLoad;
        static u32 sLastTris;
        extern volatile u64 dkr_afx_load_ns;
        extern s32 sTransitionStatus;
        u64 afxLoad = dkr_afx_load_ns;
        // The frame that opened the scene gate carries the scene's first logic
        // step, which is part of the load (see scene_gate.c).
        int opening = pc_scene_take_opening_frame();

        if (work > 20000 && pc_scene_is_ready() && !opening) {
            pc_prof_log("GFX LONG FRAME %lu us: dl %lu us, %lu texture loads in %lu us, %lu model loads | logic %lu "
                   "scene %lu us, %lu tris, transition %ld, audio loads %lu us\n",
                   (unsigned long) work, (unsigned long) ((t2 - t1) / 1000u), (unsigned long) sTexLoadsInFrame,
                   (unsigned long) (sTexLoadNsInFrame / 1000u), (unsigned long) sModelLoadsInFrame,
                   // The profile restarts its sums every PROF_FRAMES frames.
                   (unsigned long) (PROF_SINCE(sProf.part[PC_PROF_LOGIC], sLastPart[PC_PROF_LOGIC]) / 1000u),
                   (unsigned long) (PROF_SINCE(sProf.part[PC_PROF_SCENE], sLastPart[PC_PROF_SCENE]) / 1000u),
                   (unsigned long) PROF_SINCE(sProf.tris, sLastTris), (long) sTransitionStatus,
                   (unsigned long) ((afxLoad - sLastAfxLoad) / 1000u));
        }
        memcpy(sLastPart, sProf.part, sizeof(sLastPart));
        sLastTris = sProf.tris;
        sLastAfxLoad = afxLoad;
        sTexLoadsInFrame = 0;
        sTexLoadNsInFrame = 0;
        sModelLoadsInFrame = 0;
        sFrameStart = t3;
        sProf.waitFrame = 0;
    }
    if (sProf.sentFrame > sProf.maxSent) sProf.maxSent = sProf.sentFrame;
    sProf.sentFrame = 0;
    {
        pvr_stats_t stats;
        if (!pvr_get_stats(&stats)) {
            sProf.pvrRender += stats.rnd_last_time;
            sProf.pvrRegister += stats.reg_last_time;
            if (stats.vtx_buffer_used > sProf.pvrBytes) sProf.pvrBytes = (u32) stats.vtx_buffer_used;
        }
    }
    if (++sProf.frames < PROF_FRAMES) return;
    if (sProf.last) {
        u32 n = sProf.frames;
        u32 period = (u32) ((t3 - sProf.last) / 1000u / n), wait = (u32) (sProf.wait / 1000u / n);
        u32 begin = (u32) (sProf.begin / 1000u / n), dl = (u32) (sProf.dl / 1000u / n);
        u32 end = (u32) (sProf.end / 1000u / n);
        pc_prof_log("GFX PROF: frame=%lu us wait=%lu game=%lu dl=%lu begin=%lu end=%lu | batches=%lu+%lu verts=%lu "
               "tris=%lu culled=%lu clipped=%lu sent=%lu max=%lu slow=%lu rects=%lu | "
               "pvr render=%lu register=%lu us vtxbytes=%lu ptwords=%lu trwords=%lu | "
               "us blast=%lu fallback=%lu vtx=%lu poly=%lu rect=%lu mtx=%lu | "
               "us state=%lu xform=%lu emit=%lu pstate=%lu pemit=%lu pdraw=%lu slowbatches=%lu | "
               "slowtris combiner=%lu billboard=%lu state=%lu full=%lu append=%lu shared=%lu loose=%lu | "
               "workmax=%lu missed=%lu vramfree=%lu texdecodes=%lu texprebuilt=%lu | "
               "us logic=%lu scene=%lu waves=%lu shadows=%lu viewport=%lu hud=%lu | "
               "statetris noscene=%lu texenv=%lu alphatest=%lu nodepth=%lu decal=%lu scissor=%lu | "
               "nodepth toop=%lu afterdepth=%lu afterrecorded=%lu notopaque=%lu | "
               "trbatches 2d=%lu opaquez=%lu opaquenoz=%lu cutout=%lu xlu=%lu "
               "trroom 2d=%lu opaquez=%lu opaquenoz=%lu cutout=%lu xlu=%lu\n",
               (unsigned long) period, (unsigned long) wait,
               (unsigned long) (period - wait - begin - dl - end), (unsigned long) dl,
               (unsigned long) begin, (unsigned long) end,
               (unsigned long) (sProf.blast / n), (unsigned long) (sProf.fallback / n),
               (unsigned long) (sProf.verts / n), (unsigned long) (sProf.tris / n),
               (unsigned long) (sProf.culled / n), (unsigned long) (sProf.clipped / n),
               (unsigned long) (sProf.sent / n), (unsigned long) sProf.maxSent,
               (unsigned long) (sProf.slowTris / n), (unsigned long) (sProf.rects / n),
               (unsigned long) (sProf.pvrRender / 1000u / n), (unsigned long) (sProf.pvrRegister / 1000u / n),
               (unsigned long) sProf.pvrBytes, (unsigned long) sProf.ptWords,
               (unsigned long) sProf.trWords,
               (unsigned long) (sProf.tBlast / 1000u / n), (unsigned long) (sProf.tFallback / 1000u / n),
               (unsigned long) (sProf.tVtx / 1000u / n), (unsigned long) (sProf.tPoly / 1000u / n),
               (unsigned long) (sProf.tRect / 1000u / n), (unsigned long) (sProf.tMtx / 1000u / n),
               (unsigned long) (sProf.tState / 1000u / n), (unsigned long) (sProf.tXform / 1000u / n),
               (unsigned long) (sProf.tEmit / 1000u / n), (unsigned long) (sProf.tPolyState / 1000u / n),
               (unsigned long) (sProf.tPolyEmit / 1000u / n), (unsigned long) (sProf.tPolyDraw / 1000u / n),
               (unsigned long) (sProf.slowBatches / n),
               (unsigned long) (sProf.whyCombiner / n), (unsigned long) (sProf.whyBillboard / n),
               (unsigned long) (sProf.whyState / n), (unsigned long) (sProf.whyFull / n),
               (unsigned long) (sProf.whyAppend / n), (unsigned long) (sProf.whyShared / n),
               (unsigned long) (sProf.whyLoose / n),
               (unsigned long) sProf.workMax, (unsigned long) sProf.missed,
               (unsigned long) pvr_mem_available(),
               (unsigned long) sTexDecodes, (unsigned long) sTexPrebuiltHits,
               (unsigned long) (sProf.part[PC_PROF_LOGIC] / 1000u / n),
               (unsigned long) (sProf.part[PC_PROF_SCENE] / 1000u / n),
               (unsigned long) (sProf.part[PC_PROF_WAVES] / 1000u / n),
               (unsigned long) (sProf.part[PC_PROF_SHADOWS] / 1000u / n),
               (unsigned long) (sProf.part[PC_PROF_VIEWPORT] / 1000u / n),
               (unsigned long) (sProf.part[PC_PROF_HUD] / 1000u / n),
               (unsigned long) (sProf.whyStateKind[0] / n), (unsigned long) (sProf.whyStateKind[1] / n),
               (unsigned long) (sProf.whyStateKind[2] / n), (unsigned long) (sProf.whyStateKind[3] / n),
               (unsigned long) (sProf.whyStateKind[4] / n), (unsigned long) (sProf.whyStateKind[5] / n),
               (unsigned long) (sProf.noDepth[0] / n), (unsigned long) (sProf.noDepth[1] / n),
               (unsigned long) (sProf.noDepth[2] / n), (unsigned long) (sProf.noDepth[3] / n),
               (unsigned long) (sProf.trStats[0] / n), (unsigned long) (sProf.trStats[1] / n),
               (unsigned long) (sProf.trStats[2] / n), (unsigned long) (sProf.trStats[3] / n),
               (unsigned long) (sProf.trStats[4] / n), (unsigned long) (sProf.trStats[5] / n),
               (unsigned long) (sProf.trStats[6] / n), (unsigned long) (sProf.trStats[7] / n),
               (unsigned long) (sProf.trStats[8] / n), (unsigned long) (sProf.trStats[9] / n));
    }
    prof_blend_print(sProf.frames);
    sTexDecodes = sTexPrebuiltHits = 0;
    memset(&sProf, 0, sizeof(sProf));
    sProf.last = pc_uptime_ns(); // after the print, so console time is in no bucket
}
#else
#define PROF_ADD(field, n) ((void) 0)
#define PROF_TIME(field, call) call
#define PROF_LAP_START() ((void) 0)
#define PROF_LAP(field) ((void) 0)
#endif

static s32 sRetraceReset;

/**
 * Makes the next frame start the pacer's clock afresh. The scene gate calls this
 * when a scene has loaded (dreamcast/scene_gate.c): the time a load took is not
 * game time, and counting it would have the game pay it back by running the
 * first frames of the new scene too fast.
 */
void pc_retrace_reset(void) {
    sRetraceReset = TRUE;
}

#if DKR_VBL_PACER
// The vblank pacer (VBL_PACER=1): waits for the video interrupt itself instead
// of sleeping until a clock time. nanosleep overshot its target by up to 5 ms,
// which pushed frames that were close to the limit a whole field late. Game
// speed then follows the real refresh rate (59.94 Hz on NTSC, as on the N64).
static volatile u32 sPacerVbl;
static semaphore_t sPacerSem = SEM_INITIALIZER(0);

static void pacer_vblank(u32 code, void *data) {
    (void) code;
    (void) data;
    sPacerVbl++;
    sem_signal(&sPacerSem);
}

s32 pc_retrace_wait(void) {
    static s32 sStarted;
    static u32 sLastVbl;
    s32 periods;

    if (!sStarted) {
        sStarted = TRUE;
        vblank_handler_add(&pacer_vblank, NULL);
        sLastVbl = sPacerVbl;
        sRetraceReset = FALSE;
        return PC_LOGIC_UPDATE_RATE;
    }
    if (sRetraceReset) {
        sRetraceReset = FALSE;
        sLastVbl = sPacerVbl;
        return PC_LOGIC_UPDATE_RATE;
    }
#if DKR_GFX_PROFILE
    {
        u64 slept = pc_uptime_ns();
#endif
        // Every vblank signals the semaphore, so none is lost between the test
        // and the wait; signals left over from busy frames just drain here.
        while ((u32) (sPacerVbl - sLastVbl) < (u32) PC_LOGIC_UPDATE_RATE) {
            sem_wait(&sPacerSem);
        }
#if DKR_GFX_PROFILE
        slept = pc_uptime_ns() - slept;
        sProf.wait += slept;
        sProf.waitFrame += slept;
    }
#endif
    periods = (s32) (sPacerVbl - sLastVbl);
    if (periods > PC_MAX_UPDATE_RATE) {
        // A disc read in a menu or a paused debugger: not the game's time.
        sLastVbl = sPacerVbl;
        return PC_LOGIC_UPDATE_RATE;
    }
#if DKR_GFX_PROFILE
    if (periods > PC_LOGIC_UPDATE_RATE) {
        sProf.missed++;
    }
#endif
    sLastVbl += (u32) periods;
    return periods;
}
#else
s32 pc_retrace_wait(void) {
    static u64 sLastNs = 0;
    u64 targetNs;
    u64 nowNs;
    s32 periods;

    nowNs = pc_uptime_ns();

    if (sLastNs == 0 || sRetraceReset) {
        sRetraceReset = FALSE;
        sLastNs = nowNs;
        return PC_LOGIC_UPDATE_RATE;
    }

    // Sleep until this frame has covered its full committed cadence. This is
    // the stand-in for retail's blocking osRecvMesg on the vblank queue.
    targetNs = sLastNs + (PC_LOGIC_UPDATE_RATE * PC_RETRACE_NSEC);
    if (nowNs < targetNs) {
        struct timespec req;
        req.tv_sec = (targetNs - nowNs) / 1000000000ll;
        req.tv_nsec = (targetNs - nowNs) % 1000000000ll;
        nanosleep(&req, NULL);
#if DKR_GFX_PROFILE
        {
            u64 woke = pc_uptime_ns();
            u64 slept = woke - nowNs;
            sProf.wait += slept;
            sProf.waitFrame += slept;
            if (woke > targetNs) {
                sScene.overNs += woke - targetNs;
                if (woke - targetNs > sScene.maxOverNs) sScene.maxOverNs = woke - targetNs;
            }
        }
#endif
        nowNs = targetNs;
    }

    periods = (s32) ((nowNs - sLastNs) / PC_RETRACE_NSEC);
    if (periods < PC_LOGIC_UPDATE_RATE) {
        periods = PC_LOGIC_UPDATE_RATE;
    }
    if (periods > PC_MAX_UPDATE_RATE) {
        // Far more time went by than a frame can account for, outside a scene
        // change: a disc read in a menu, or a paused debugger. That time is not
        // the game's either. Start counting again from now; advancing by the cap
        // alone would leave the rest owed, and every following frame would run
        // at the cap until it was repaid.
        sLastNs = nowNs;
        return PC_LOGIC_UPDATE_RATE;
    }

#if DKR_GFX_PROFILE
    // Counted here because only the pacer knows: a frame's own time includes
    // waiting for the PVR, so comparing it with 16.7 ms says nothing.
    if (periods > PC_LOGIC_UPDATE_RATE) {
        sProf.missed++;
    }
#endif
    // Advance by whole periods so rounding doesn't accumulate into drift.
    sLastNs += (long long) periods * PC_RETRACE_NSEC;
    return periods;
}
#endif

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define WINDOW_SCALE 3

#define GFX_MAX_VERTS 64    // the RSP's internal vertex array
#define GFX_MAX_DEPTH 16    // display-list recursion guard
// One G_TRIN is at most 16 triangles, each at most two after the near clip, so
// a batch never stages more than 96 corners; the memory matters more here.
#define GFX_MAX_TRI_VERTS 1024

// A vertex in clip space: post-matrix, pre-divide. Kept unprojected because
// near-plane clipping interpolates here, before the divide by w (a vertex behind
// the camera has w <= 0, and dividing by it is the garbage the clip prevents).
// Field order: everything project() reads on the common path is at the front, so
// a vertex is one 32-byte line to shade. clip[] is only touched when a triangle
// crosses the near plane, so it sits at the back with the cold data.
typedef struct {
    // The perspective divide + viewport map, computed once at G_VTX time rather
    // than three-times-per-triangle in push_tri, as the RSP does it: the vertex
    // command projects, the triangle commands reference the result. Display lists
    // reuse each vertex across two or three triangles, so this beats per-corner.
    // `sw` doubles as the "is this projected" flag, keeping the hot set at 32
    // bytes not 33. A projected vertex always has sw == clip[3] >= GFX_NEAR_W
    // (1.0f), so zero is unreachable; project_into/clip_edge store 0.0f for
    // "behind the near plane, nothing cached". Read it via VERTEX_PROJECTED().
    f32 sx, sy, sz, sw, sfog; // 20
    f32 u, v;                 // 28 — per-triangle, written by handle_polygon
    u8 r, g, b, a;            // 32 — shade

    f32 clip[4]; // cold: only the clip path reads this
} __attribute__((aligned(32))) GfxVertex;


// Clip anything closer than this. The N64 clips against w, and w is the
// camera-space depth, so this is the near plane in world units.
#define GFX_NEAR_W 1.0f

// Whether a vertex carries a cached projection. See GfxVertex.sw.
#define VERTEX_PROJECTED(v) ((v)->sw != 0.0f)

// Which screen-space winding is a front face. Screen y runs downwards, so this is
// the opposite sign from the y-up convention. Flip if the world renders
// inside-out (outer surfaces gone, inner ones visible).
#define GFX_FRONT_FACE_SIGN (-1.0f)

static u32 sGfxFrameCount = 0;

alignas(32) static f32 sMatrices[3][4][4]; // G_MTX_DKR_INDEX_0..2
static s32 sCurMatrix = 0;
static s32 sBillboard = FALSE;

_Static_assert(sizeof(GfxTriVert) == 32, "GfxTriVert must be exactly one SH4 cache line");
_Static_assert(__builtin_offsetof(GfxVertex, clip) == 32, "GfxVertex hot fields must fit one cache line");

static GfxVertex sVerts[GFX_MAX_VERTS];
static s32 sVertexBase = 0; // where G_VTX_APPEND vertices land

static f32 sVpScaleX = N64_SCREEN_W / 2.0f;
static f32 sVpScaleY = N64_SCREEN_H / 2.0f;
static f32 sVpTransX = N64_SCREEN_W / 2.0f;
static f32 sVpTransY = N64_SCREEN_H / 2.0f;

// ---------------------------------------------------------------------------
// What gets clipped away, in screen pixels, x1/y1 exclusive.
//
// Two rectangles, and the drawing is confined to the intersection:
//
//   sScis*  — the RDP scissor (G_SETSCISSOR). Split-screen, text boxes.
//   sVpClip* — the viewport's own bounds. The RSP clips to the view volume, and
//       the viewport maps NDC +-1 onto this rectangle, so on hardware nothing
//       draws outside it. This path only clips against the near plane, so without
//       this a small viewport (the level-select track-preview window) spills its
//       geometry across the whole screen.
// ---------------------------------------------------------------------------
static f32 sScisX0, sScisY0, sScisX1, sScisY1;
static f32 sVpClipX0, sVpClipY0, sVpClipX1, sVpClipY1;

static f32 max_f(f32 a, f32 b) {
    return (a > b) ? a : b;
}

static f32 min_f(f32 a, f32 b) {
    return (a < b) ? a : b;
}

static void apply_clip_rect(void) {
    gfx_set_scissor(max_f(sScisX0, sVpClipX0), max_f(sScisY0, sVpClipY0), min_f(sScisX1, sVpClipX1),
                    min_f(sScisY1, sVpClipY1));
}

static GfxTriVert sTriVerts[GFX_MAX_TRI_VERTS];
static s32 sTriVertCount = 0;

static u32 sTimgAddr = 0;  // G_SETTIMG — wherever the DL last pointed
static u32 sTexAddr = 0;   // the image a G_LOADBLOCK/G_LOADTILE actually loaded
static s32 sTexSwapped = 0; // that load had dxt == 0 — see decode_texture()
static u8 sTileFmt = 0;    // G_SETTILE — G_IM_FMT_*
static u8 sTileSiz = 0;    //             G_IM_SIZ_*
static u8 sTilePalette = 0;
static u8 sTileCmS = 0; // the raw 2-bit clamp/mirror fields, not booleans
static u8 sTileCmT = 0;
static u16 sTileWidth = 0; // G_SETTILESIZE
static u16 sTileHeight = 0;
static u16 sTileUls = 0;   //               the tile's origin within the image
static u16 sTileUlt = 0;
static u32 sTlutAddr = 0;  // G_LOADTLUT — palette for the CI formats

#define GFX_GEOMETRY_MODE_INIT (G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER)
static u32 sGeometryMode = GFX_GEOMETRY_MODE_INIT;

static u32 sOtherModeH = 0;      // G_SETOTHERMODE_H / G_RDPSETOTHERMODE
static u32 sOtherModeL = 0;      // G_SETOTHERMODE_L / G_RDPSETOTHERMODE
static u32 sCombineW0 = 0;       // G_SETCOMBINE — the two mux words
static u32 sCombineW1 = 0;
static u8 sPrimColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sEnvColor[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static u8 sFillColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static u8 sBlendColor[4] = { 0x00, 0x00, 0x00, 0xFF }; // G_SETBLENDCOLOR

// Fog. The game recomputes this every frame, per player (src/tracks.c), out of the
// FogData system its fog-changer objects drive. gSPFogPosition packs a multiplier
// and an offset into one G_MOVEWORD, and the RSP turns them into a per-vertex fade
// factor; gDPSetFogColor is the colour that factor fades towards. 
static u8 sFogColor[4] = { 0x00, 0x00, 0x00, 0xFF };
static s16 sFogMul = 0;
static s16 sFogOfs = 0;

#define GFX_MAX_TEXTURES 1024
#define GFX_MAX_TEX_TEXELS (256 * 256)

typedef struct {
    u32 timg;
    u32 tlut;
    u8 fmt, siz, cmS, cmT;
    u8 swapped;
    u16 width, height;
    u32 handle;
    u32 lastUsed; // frame number, for eviction when the cache is full
    u8 prebuilt;  // the handle belongs to the prebuilt table, not to this entry
} GfxTexture;

static GfxTexture sTexCache[GFX_MAX_TEXTURES];
static s32 sTexCacheCount = 0;

// Hash hint over the cache: hash(timg, tlut) -> sTexCache index + 1 (0 = no
// hint). Purely an accelerator: every hint is verified against the full key
// before use, and a wrong one (hash collision, entry moved by eviction or
// invalidation) just means one linear scan that re-teaches the slot. So the
// eviction paths never need to maintain it.
#define GFX_TEX_HINTS 1024
static u16 sTexHint[GFX_TEX_HINTS];

static u32 tex_hint_slot(u32 timg, u32 tlut) {
    return ((timg >> 5) ^ (timg >> 15) ^ (tlut >> 5)) & (GFX_TEX_HINTS - 1);
}

#define GFX_TEX_HINT_PROBES 4

/** Remember cache entry `index` near `slot`: in a free or stale hint if there is one. */
static void tex_hint_store(u32 slot, s32 index) {
    static u32 sNext;
    s32 probe;

    for (probe = 0; probe < GFX_TEX_HINT_PROBES; probe++) {
        u32 at = (slot + probe) & (GFX_TEX_HINTS - 1);
        s32 hi = (s32) sTexHint[at] - 1;

        if (hi < 0 || hi >= sTexCacheCount || hi == index ||
            ((tex_hint_slot(sTexCache[hi].timg, sTexCache[hi].tlut) ^ slot) & (GFX_TEX_HINTS - 1)) != 0) {
            sTexHint[at] = (u16) (index + 1);
            return;
        }
    }
    sTexHint[(slot + (sNext++ % GFX_TEX_HINT_PROBES)) & (GFX_TEX_HINTS - 1)] = (u16) (index + 1);
}
static u32 sTexDecodeBuf[GFX_MAX_TEX_TEXELS];
static u8 sTexSwizzleBuf[GFX_MAX_TEX_TEXELS * 4]; // worst case: 32bpp


static s32 sTexStateDirty;

// ---------------------------------------------------------------------------
// Prebuilt textures
//
// dreamcast/build_pvr_textures.py converts every texture asset offline into
// build/dc/textures.dtp. When the game loads a texture asset (load_texture,
// during a scene's load), enDjinn loads its frames from that pack into video
// memory, keyed by the address their texels have in RAM, which is the address
// the display list will name. texture_lookup then finds them without decoding
// anything; decoding stays only for what the pack does not cover.
// ---------------------------------------------------------------------------
#define PREBUILT_MAX 2048 // texture frames resident at once; a race level loads about 950
static struct {
    u32 timg;   // the frame's texels in RAM: the texture header plus one
    u32 handle;
    u16 width, height;
    s16 bundle; // the sBundle entry that owns the handle, or -1 if this entry owns it
} sPrebuilt[PREBUILT_MAX];

// The current scene's bundle (build/dc/texscenes/LEVEL.dtb): every texture the
// scene is known to use, loaded into video memory in one sequential read when
// the scene starts loading. load_texture then claims its frames from here
// instead of reading the pack. The bundle keeps its textures for the whole
// scene: one the game frees goes back to the bundle, so loading it again (the
// character demo swaps models) does not read the pack. The next scene's load
// frees what is unclaimed and hands what is still in use to sPrebuilt.
#define BUNDLE_MAX 2048
static struct {
    u16 id, frame;
    u16 width, height;
    u32 handle;
    s16 claimed; // in use by an sPrebuilt entry
} sBundle[BUNDLE_MAX];
static s32 sBundleCount;
static s32 sSceneLevel = -1;
static u32 sBundleClaims; // textures taken from the bundle since the last report
static u32 sModelHeld, sModelHeldMs; // scene models the gate loaded, see pc_model_scene_load
static s32 sPrebuiltCount;
static file_t sPackFile = FILEHND_INVALID;
static u32 *sPackIndex;
static u32 sPackCount2d, sPackCount3d, sPackSize;
static s32 sPackTried;

static s32 prebuilt_open(void) {
    u32 header[4];

    if (sPackTried) {
        return sPackIndex != NULL;
    }
    sPackTried = TRUE;
    sPackFile = fs_open(DKR_TEXTURE_PACK, O_RDONLY);
    if (sPackFile == FILEHND_INVALID) {
        printf("gfx: no %s; textures are decoded as they are drawn\n", DKR_TEXTURE_PACK);
        return FALSE;
    }
    if (fs_read(sPackFile, header, sizeof(header)) != sizeof(header) || header[0] != 0x4B505444u /* DTPK */ ||
        header[1] != 2) {
        printf("gfx: %s is not a version 2 texture pack\n", DKR_TEXTURE_PACK);
        return FALSE;
    }
    sPackCount2d = header[2];
    sPackCount3d = header[3];
    sPackSize = (u32) fs_total(sPackFile);
    sPackIndex = malloc((sPackCount2d + sPackCount3d) * sizeof(u32));
    if (sPackIndex == NULL || fs_read(sPackFile, sPackIndex, (sPackCount2d + sPackCount3d) * sizeof(u32)) !=
                                  (ssize_t) ((sPackCount2d + sPackCount3d) * sizeof(u32))) {
        free(sPackIndex);
        sPackIndex = NULL;
        return FALSE;
    }
    return TRUE;
}

/** Drop prebuilt frames whose texels lay in freed memory. */
static void prebuilt_forget(u32 lo, u32 hi) {
    s32 i;

    for (i = 0; i < sPrebuiltCount;) {
        if (sPrebuilt[i].timg >= lo && sPrebuilt[i].timg < hi) {
            if (sPrebuilt[i].bundle >= 0) {
                sBundle[sPrebuilt[i].bundle].claimed = FALSE;
            } else {
                gfx_delete_texture(sPrebuilt[i].handle);
            }
            sPrebuilt[i] = sPrebuilt[--sPrebuiltCount];
        } else {
            i++;
        }
    }
}

static s32 prebuilt_find(u32 timg) {
    s32 i;

    for (i = 0; i < sPrebuiltCount; i++) {
        if (sPrebuilt[i].timg == timg) {
            return i;
        }
    }
    return -1;
}

/**
 * Called by load_texture once a texture asset is in RAM: puts its prebuilt
 * frames in video memory. id is the asset ID (bit 15 set for the 3D table),
 * first the first frame's header; frames follow textureSize bytes apart.
 */
/** Registers frame `i` of a texture whose header is at `frame`; returns FALSE if the table is full. */
static s32 prebuilt_register(TextureHeader *frame, u32 handle, u16 width, u16 height, s16 bundle) {
    u32 timg = (u32) (frame + 1);

    if (sPrebuiltCount >= PREBUILT_MAX) {
        return FALSE;
    }
    prebuilt_forget(timg, timg + 1); // memory reused for a new asset
    sPrebuilt[sPrebuiltCount].timg = timg;
    sPrebuilt[sPrebuiltCount].handle = handle;
    sPrebuilt[sPrebuiltCount].width = width;
    sPrebuilt[sPrebuiltCount].height = height;
    sPrebuilt[sPrebuiltCount].bundle = bundle;
    sPrebuiltCount++;
    return TRUE;
}

/** Takes a texture's frames from the scene bundle if they are all there. */
static s32 bundle_claim(void *first, s32 id, s32 frames) {
    TextureHeader *frame = (TextureHeader *) first;
    s32 found = 0, i, f;

    for (i = 0; i < sBundleCount; i++) {
        if (!sBundle[i].claimed && sBundle[i].id == (u16) id) {
            found++;
        }
    }
    if (found != frames) {
        return FALSE;
    }
    for (f = 0; f < frames; f++) {
        for (i = 0; i < sBundleCount; i++) {
            if (!sBundle[i].claimed && sBundle[i].id == (u16) id && sBundle[i].frame == f) {
                if (sBundle[i].width != frame->width || sBundle[i].height != frame->height ||
                    !prebuilt_register(frame, sBundle[i].handle, sBundle[i].width, sBundle[i].height, (s16) i)) {
                    return f > 0; // a partial claim keeps what it took
                }
                sBundle[i].claimed = TRUE;
                break;
            }
        }
        frame = (TextureHeader *) ((u8 *) frame + frame->textureSize);
    }
    sBundleClaims++;
    return TRUE;
}

/**
 * The scene gate calls this as a scene starts loading: frees what the previous
 * scene's bundle has unclaimed, hands what is claimed to sPrebuilt, then reads
 * this scene's bundle straight through and puts every texture in it into video
 * memory.
 */
void pc_texture_scene_load(int level) {
    char path[96];
    u32 header[4];
    u32 pos;
    file_t file;
    s32 i, n;

    for (i = 0; i < sBundleCount; i++) {
        if (!sBundle[i].claimed) {
            gfx_delete_texture(sBundle[i].handle);
        }
    }
    for (i = 0; i < sPrebuiltCount; i++) {
        sPrebuilt[i].bundle = -1;
    }
    sBundleCount = 0;
    sSceneLevel = level;
    snprintf(path, sizeof(path), DKR_ASSET_MOUNT "/build/dc/texscenes/%ld.dtb", (long) level);
    file = fs_open(path, O_RDONLY);
    if (file == FILEHND_INVALID) {
        return;
    }
    if (fs_read(file, header, sizeof(header)) != sizeof(header) || header[0] != 0x42535444u /* DTSB */ ||
        header[1] != 1) {
        fs_close(file);
        return;
    }
    pos = sizeof(header);
    for (n = 0; n < (s32) header[2] && sBundleCount < BUNDLE_MAX; n++) {
        u32 entry[2];
        u16 *record;
        u32 *table;
        u8 *block;

        // Each entry header sits 8 bytes before a 32-byte boundary, so its
        // record starts aligned for enDjinn's loader.
        while ((pos + 8) % 32) {
            u8 pad[32];
            u32 skip = (32 - ((pos + 8) % 32)) % 32;
            if (fs_read(file, pad, skip) != (ssize_t) skip) {
                goto done;
            }
            pos += skip;
        }
        if (fs_read(file, entry, sizeof(entry)) != sizeof(entry)) {
            break;
        }
        pos += sizeof(entry);
        block = memalign(32, entry[1]);
        if (block == NULL) {
            break;
        }
        if (fs_read(file, block, entry[1]) != (ssize_t) entry[1]) {
            free(block);
            break;
        }
        pos += entry[1];
        record = (u16 *) block;
        table = (u32 *) (block + 8);
        for (i = 0; i < record[2] && sBundleCount < BUNDLE_MAX; i++) {
            enj_texture_info_t info;
            u32 handle = 0;

            if (table[2 * i] + table[2 * i + 1] <= entry[1] && enj_texture_load_blob(block + table[2 * i], &info)) {
                handle = gfx_adopt_texture(info.ptr, record[0], record[1], info.width, info.height,
                                           info.pvrformat & 0x7FFFFFFFu, info.flags.mipmapped, record[3]);
                if (handle == 0) {
                    pvr_mem_free(info.ptr);
                }
            }
            if (handle == 0) {
                printf("gfx: scene %ld bundle stopped at texture %04lx frame %ld, %lu bytes of video memory free\n",
                       (long) level, (unsigned long) entry[0], (long) i, (unsigned long) pvr_mem_available());
                break;
            }
            sBundle[sBundleCount].id = (u16) entry[0];
            sBundle[sBundleCount].frame = (u16) i;
            sBundle[sBundleCount].width = record[0];
            sBundle[sBundleCount].height = record[1];
            sBundle[sBundleCount].handle = handle;
            sBundle[sBundleCount].claimed = FALSE;
            sBundleCount++;
        }
        free(block);
    }
done:
    fs_close(file);
}

// Time and reads spent loading prebuilt textures since the last scene report.
static u64 sTexLoadNs;
static u32 sTexLoadReads, sTexLoadCount;

void pc_texture_loaded(void *first, s32 id, s32 frames) {
    u16 *record;
    u32 *table;
    u32 offset, index, start, end, bytes;
    u8 *chunk;
    TextureHeader *frame = (TextureHeader *) first;
    u64 started = timer_ns_gettime64();
    s32 i;

#if DKR_TEX_TRACE
    printf("TEXUSE level=%ld id=%04lx\n", (long) sSceneLevel, (unsigned long) (id & 0xFFFF));
#endif
    if (bundle_claim(first, id, frames)) {
        return;
    }
    if (!prebuilt_open()) {
        return;
    }
    if ((id & 0x8000) ? (u32) (id & 0x7FFF) >= sPackCount3d : (u32) id >= sPackCount2d) {
        return;
    }
    index = (id & 0x8000) ? sPackCount2d + (u32) (id & 0x7FFF) : (u32) id;
    offset = sPackIndex[index];
    if (offset == 0 || frames <= 0) {
#if DKR_TEX_TRACE
        printf("TEX LOAD id=%04lx frames=%ld at %08lx: %s\n", (unsigned long) id, (long) frames,
               (unsigned long) (frame + 1), offset == 0 ? "not in the pack" : "no frames");
#endif
        return;
    }
    // A texture's record, frame table and frames lie together in the pack, and
    // the next texture's record starts where they end (sPackIndex[index + 1],
    // or the end of the file). Read all of it at once: a seek on disc costs far
    // more than the bytes.
    end = 0;
    for (i = (s32) index + 1; i < (s32) (sPackCount2d + sPackCount3d) && end == 0; i++) {
        end = sPackIndex[i];
    }
    if (end == 0) {
        end = sPackSize;
    }
    start = offset;
    bytes = end - start;
    chunk = memalign(32, bytes);
    if (chunk == NULL) {
        return;
    }
    sTexLoadReads++;
    if (fs_seek(sPackFile, start, SEEK_SET) < 0 || fs_read(sPackFile, chunk, bytes) != (ssize_t) bytes) {
        free(chunk);
        return;
    }
    record = (u16 *) chunk;
    table = (u32 *) (chunk + 8);
    if (bytes < 8u + 8u * (u32) frames || record[2] != (u16) frames || record[0] != frame->width ||
        record[1] != frame->height) {
#if DKR_TEX_TRACE
        printf("TEX LOAD id=%04lx at %08lx: pack %ux%u x%u, asset %ux%u x%ld\n", (unsigned long) id,
               (unsigned long) (frame + 1), record[0], record[1], record[2], frame->width, frame->height, (long) frames);
#endif
        free(chunk);
        return; // not the asset the pack was built from
    }
    for (i = 0; i < frames && sPrebuiltCount < PREBUILT_MAX; i++) {
        u32 timg = (u32) (frame + 1);
        u32 at = table[2 * i] - start;
        enj_texture_info_t info;
        u32 handle = 0;

        // A whole pvrtex DcTx file, 32-byte aligned within the pack and so
        // within this chunk; enDjinn reads its header and loads the rest.
        if (at + table[2 * i + 1] <= bytes && enj_texture_load_blob(chunk + at, &info)) {
            handle = gfx_adopt_texture(info.ptr, record[0], record[1], info.width, info.height,
                                       info.pvrformat & 0x7FFFFFFFu, info.flags.mipmapped, record[3]);
            if (handle == 0) {
                pvr_mem_free(info.ptr);
            }
        }
        if (handle == 0) {
            break;
        }
        prebuilt_forget(timg, timg + 1); // memory reused for a new asset
        sPrebuilt[sPrebuiltCount].timg = timg;
        sPrebuilt[sPrebuiltCount].handle = handle;
        sPrebuilt[sPrebuiltCount].width = record[0];
        sPrebuilt[sPrebuiltCount].height = record[1];
        sPrebuilt[sPrebuiltCount].bundle = -1;
        sPrebuiltCount++;
        sTexLoadCount++;
        frame = (TextureHeader *) ((u8 *) frame + frame->textureSize);
    }
    free(chunk);
    sTexLoadNs += timer_ns_gettime64() - started;
    if (pc_scene_is_ready()) {
        // A texture loaded while the scene runs: a disc read in the middle of play.
        sTexLoadsInFrame++;
        sTexLoadNsInFrame += timer_ns_gettime64() - started;
    }
}

/** Prints and resets what loading prebuilt textures cost; the scene gate calls it. */
void pc_texture_load_report(u32 sceneMs) {
    s32 i, unclaimed = 0;

    for (i = 0; i < sBundleCount; i++) {
        unclaimed += !sBundle[i].claimed;
    }
    printf("SCENE LOAD %lu ms: %lu textures from the scene bundle (%ld frames left unclaimed), "
           "%lu frames read singly in %lu ms, %lu models held in %lu ms\n", (unsigned long) sceneMs,
           (unsigned long) sBundleClaims, (long) unclaimed, (unsigned long) sTexLoadCount,
           (unsigned long) (sTexLoadNs / 1000000u), (unsigned long) sModelHeld, (unsigned long) sModelHeldMs);
    sTexLoadNs = 0;
    sTexLoadReads = sTexLoadCount = sBundleClaims = 0;
}

// ---------------------------------------------------------------------------
// Scene models. dreamcast/models.scenes lists, per scene, the object models
// its objects load after the scene has started (the character demo swaps
// racers). The scene gate loads them and holds a reference until the next
// scene loads, so those objects find them in the game's model cache.
// ---------------------------------------------------------------------------
#define DKR_MODEL_SCENES DKR_ASSET_MOUNT "/build/dc/models.scenes"
extern s32 pc_model_hold(s32 modelID);
extern void pc_model_release_held(void);

void pc_model_loaded(s32 modelID) {
#if DKR_TEX_TRACE
    printf("MODELUSE level=%ld id=%04lx\n", (long) sSceneLevel, (unsigned long) modelID);
#endif
    if (pc_scene_is_ready()) {
        sModelLoadsInFrame++; // a disc read in the middle of play
    }
}

void pc_model_scene_release(void) {
    pc_model_release_held();
}

/** Reads a scene map (build/dc/NAME.scenes) once; NULL if it is missing. */
static const char *scene_map(const char *path, char **cache, s32 *tried) {
    if (!*tried) {
        file_t file = fs_open(path, O_RDONLY);
        *tried = TRUE;
        if (file != FILEHND_INVALID) {
            size_t size = fs_total(file);
            *cache = malloc(size + 1);
            if (*cache != NULL && fs_read(file, *cache, size) == (ssize_t) size) {
                (*cache)[size] = '\0';
            } else {
                free(*cache);
                *cache = NULL;
            }
            fs_close(file);
        }
    }
    return *cache;
}

/** The IDs a scene map lists for a level, after "scene LEVEL "; NULL if none. */
static const char *scene_map_ids(const char *map, int level) {
    char key[24];
    const char *line;
    s32 keyLength;

    if (map == NULL || level < 0) {
        return NULL;
    }
    keyLength = snprintf(key, sizeof(key), "scene %ld ", (long) level);
    for (line = map; line != NULL && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, keyLength) == 0) {
            return line + keyLength;
        }
    }
    return NULL;
}

void pc_model_scene_load(int level) {
    static char *sMap;
    static s32 sMapTried;
    u64 started = timer_ns_gettime64();
    const char *p = scene_map_ids(scene_map(DKR_MODEL_SCENES, &sMap, &sMapTried), level);

    sModelHeld = 0;
    while (p != NULL && *p && *p != '\n') {
        char *end;
        long id = strtol(p, &end, 16);
        if (end == p || !pc_model_hold((s32) id)) {
            break; // the end of the line, or the model table or memory has no more room
        }
        sModelHeld++;
        p = end;
    }
    sModelHeldMs = (u32) ((timer_ns_gettime64() - started) / 1000000u);
}

// dreamcast/songs.scenes: the song a scene starts after it has begun (a
// cutscene's animation object), which the gate makes resident beforehand.
#define DKR_SONG_SCENES DKR_ASSET_MOUNT "/build/dc/songs.scenes"
extern void music_prepare(u8 seqID);

void pc_song_scene_load(int level) {
    static char *sMap;
    static s32 sMapTried;
    const char *p = scene_map_ids(scene_map(DKR_SONG_SCENES, &sMap, &sMapTried), level);
    char *end;
    long sequence;

    if (p == NULL) {
        return;
    }
    sequence = strtol(p, &end, 16);
    if (end != p && sequence > 0) {
        music_prepare((u8) sequence); // one song fits; the first the scene starts
    }
}

void pc_gfx_invalidate_range(const void *addr, s32 size) {
    u32 lo = (u32) addr;
    u32 hi = lo + (u32) size;
    s32 i;

    for (i = 0; i < sTexCacheCount;) {
        GfxTexture *t = &sTexCache[i];

        if ((t->timg >= lo && t->timg < hi) || (t->tlut >= lo && t->tlut < hi)) {
            if (!t->prebuilt) {
                gfx_delete_texture(t->handle);
            }
            sTexCache[i] = sTexCache[--sTexCacheCount];
            sTexStateDirty = TRUE;
        } else {
            i++;
        }
    }
    prebuilt_forget(lo, hi);
}

/**
 * Unpack an N64 Mtx (s15.16 fixed point: the integer halves live in the first 8
 * words, the fractional halves in the last 8) into floats — the inverse of
 * mtxf_to_mtx() in src/hasm/math_util.c. Deliberately kept in the same format
 * the RSP consumes, so the PC build sees the same rounding and the same +-32768
 * saturation the console does and the N64 ROM stays a usable oracle.
 */
static void mtx_to_float(const Mtx *m, f32 out[4][4]) {
#ifdef GBI_FLOAT_MTX
    // Float matrix ABI: mtxf_to_mtx()/guMtxF2L() stored the 16 floats raw, so
    // read them back as-is. No fixed unpack, no /65536, no bit-shuffle.
    shz_mat4x4_copy((shz_mat4x4_t *) out, (const shz_mat4x4_t *) m); // paired fmov.d
#else
    const s32 *ints = (const s32 *) &m->m[0][0];
    const s32 *fracs = ints + 8;
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 idx = (i * 2) + (j >> 1);
            s32 fixed;

            if ((j & 1) == 0) {
                fixed = (ints[idx] & 0xFFFF0000) | ((fracs[idx] >> 16) & 0xFFFF);
            } else {
                fixed = (ints[idx] << 16) | (fracs[idx] & 0xFFFF);
            }
            out[i][j] = (f32) fixed / 65536.0f;
        }
    }
#endif
}

/**
 * Transform a vertex the way the RSP does: as a row vector times the selected
 * matrix (the same convention as mtxf_transform_point), then — when
 * billboarding — add the anchor's clip coordinates, then perspective divide and
 * map through the viewport.
 */
static void project_into(GfxVertex *v);

static void load_vertex(GfxVertex *dst, const Vertex *v, const f32 *anchor) {
    SHZ_ALIASING shz_vec4_t* out = (SHZ_ALIASING shz_vec4_t*)dst->clip;

    *out = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));

    if(anchor)
        *out = shz_vec4_add(*out, *(SHZ_ALIASING shz_vec4_t*)anchor);

    dst->r = v->r;
    dst->g = v->g;
    dst->b = v->b;
    // Shade alpha. The combiner uses it as a blend weight (SHADE_ALPHA), and the
    // RENDER_VTX_ALPHA materials use it as real translucency — which is why it is
    // mutually exclusive with fog in material_set(): the RSP keeps vertex alpha in
    // the fog slot.
    dst->a = v->a;

    project_into(dst);
}

/**
 * For dreamcast/scrawl.c: a world point through the current matrix, as G_VTX
 * would place it: N64 screen pixels and w. 0 if it is behind the near plane.
 */
int pc_project_world(const f32 world[3], f32 out[3]) {
    GfxVertex v;
    Vertex in = { (s16) world[0], (s16) world[1], (s16) world[2], 255, 255, 255, 255 };

    shz_xmtrx_load_4x4((shz_mat4x4_t *) &sMatrices[sCurMatrix]);
    load_vertex(&v, &in, NULL);
    if (!VERTEX_PROJECTED(&v)) {
        return 0;
    }
    out[0] = v.sx;
    out[1] = v.sy;
    out[2] = v.sw;
    return 1;
}

/**
 * For dreamcast/swirl.c: count model points, x y z at a stride of `stride`
 * floats, through the current matrix, as G_VTX would place them: N64 screen
 * pixels and w, into out. Not clipped: they must be in front of the near plane.
 */
void pc_project_points(const f32 *xyz, int stride, int count, f32 (*out)[3]) {
    int i;

    shz_xmtrx_load_4x4((shz_mat4x4_t *) &sMatrices[sCurMatrix]);
    for (i = 0; i < count; i++, xyz += stride) {
        shz_vec4_t clip = shz_xmtrx_transform_vec4(shz_vec4_init(xyz[0], xyz[1], xyz[2], 1.0f));
        f32 invW = shz_invf_fsrra(clip.w);

        out[i][0] = clip.x * invW * sVpScaleX + sVpTransX;
        out[i][1] = sVpTransY - clip.y * invW * sVpScaleY;
        out[i][2] = clip.w;
    }
}

static void handle_vertex(u32 w0, u32 w1) {
    const Vertex *src = (const Vertex *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = ((params >> 3) & 0x1F) + 1;
    s32 append = params & G_VTX_APPEND;
    s32 dstIdx;
    s32 i;

    if (src == NULL) {
        return;
    }

    if (append) {
        dstIdx = sVertexBase;
    } else {
        // Non-appended vertices always go to the start of the array, and their
        // count becomes the base that appended vertices land after.
        dstIdx = 0;
        sVertexBase = count;
    }

    PROF_ADD(verts, count);
    shz_xmtrx_load_4x4((shz_mat4x4_t*)&sMatrices[sCurMatrix]);
    for (i = 0; i < count && dstIdx + i < GFX_MAX_VERTS; i++) {
        // Billboarded vertices are sprite-space offsets from vertex 0, the
        // anchor pushed just before the billboard matrix.
        load_vertex(&sVerts[dstIdx + i], &src[i], sBillboard ? sVerts[0].clip : NULL);
    }

}

/** Expand an N64 RGBA5551 texel (big-endian in the asset) to RGBA8888. */
static u32 rgba16_to_rgba32(u16 texel) {
    u32 r = (texel >> 11) & 0x1F;
    u32 g = (texel >> 6) & 0x1F;
    u32 b = (texel >> 1) & 0x1F;
    u32 a = (texel & 1) ? 0xFF : 0x00;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return r | (g << 8) | (b << 16) | (a << 24);
}

static u32 ia_to_rgba32(u32 intensity, u32 alpha) {
    return intensity | (intensity << 8) | (intensity << 16) | (alpha << 24);
}

/**
 * Undo the N64's odd-row word swizzle.
 *
 * TMEM stores odd rows of a texture with the two 32-bit halves of each 64-bit
 * word exchanged. A normal gDPLoadTextureBlock passes a nonzero dxt, which makes
 * the RDP apply that swizzle as it streams the block in, and the texture fetch
 * unit undoes it on the way out — so the bytes in RAM are plain linear and we can
 * read them as-is.
 *
 * DKR's RENDER_LINE_SWAP textures ("Texture has swapped lines, for speed") do not
 * work that way. They go through gDPLoadTextureBlockS, whose only difference is
 * that it passes dxt = 0 (include/PR/gbi.h:2639) — so the RDP does no swizzle on
 * load, and the asset is instead stored *pre-swizzled* in ROM, letting the fetch
 * unit's unswizzle produce the correct image for free. Reading those bytes
 * linearly, as we did, leaves every odd row with its 4-byte groups exchanged in
 * pairs: the sprite stays entirely recognisable but its odd scanlines are
 * displaced in short runs, which is the serrated comb along the edges of the HUD
 * text.
 *
 * So: when the load had dxt == 0, walk the rows back through the same swap before
 * decoding. `b ^ 4` is the swizzle; it is its own inverse. Rows narrower than 8
 * bytes have nothing to exchange, and the bounds check covers a trailing partial
 * group.
 */
static void unswizzle_rows(const u8 *src, u8 *dst, u8 siz, s32 rowBytes, s32 height) {
    // The swizzle exchanges the 32-bit halves of each 64-bit TMEM word, so it is
    // a ^4 on the byte address *within TMEM*. For 4/8/16-bit texels, TMEM holds
    // the image exactly as RAM does and the ^4 carries straight over.
    
    s32 unit = (siz == G_IM_SIZ_32b) ? 8 : 4;
    s32 total = rowBytes * height;
    s32 y, b;

    for (y = 0; y < height; y++) {
        for (b = 0; b < rowBytes; b++) {
            // The ^ is against the offset into the whole image, not into the row:
            // a block load fills TMEM contiguously, and the two only coincide
            // when a row is a whole number of 64-bit words.
            s32 at = (y * rowBytes) + b;
            s32 from = (y & 1) ? (at ^ unit) : at;

            dst[at] = src[(from < total) ? from : at];
        }
    }
}

/** How many bytes one row of a `width`-texel image of this size occupies. */
static s32 row_bytes(u8 siz, s32 width) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return (width + 1) / 2;
        case G_IM_SIZ_8b:
            return width;
        case G_IM_SIZ_16b:
            return width * 2;
        default:
            return width * 4;
    }
}

/**
 * Decode one of the N64 texture formats into RGBA8888. Everything is read a
 * byte at a time, so the assets staying big-endian doesn't matter here.
 */
static void decode_texture(const u8 *src, u8 fmt, u8 siz, s32 width, s32 height, const u8 *tlut, u32 *out) {
    s32 count = width * height;
    s32 i;

    switch ((fmt << 2) | siz) {
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = rgba16_to_rgba32((src[i * 2] << 8) | src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_RGBA << 2) | G_IM_SIZ_32b:
            for (i = 0; i < count; i++) {
                out[i] = src[i * 4] | (src[(i * 4) + 1] << 8) | (src[(i * 4) + 2] << 16) | (src[(i * 4) + 3] << 24);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 idx = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_CI << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 idx = src[i];
                out[i] = rgba16_to_rgba32((tlut[idx * 2] << 8) | tlut[(idx * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_16b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i * 2], src[(i * 2) + 1]);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                u32 hi = src[i] >> 4;
                u32 lo = src[i] & 0xF;
                out[i] = ia_to_rgba32((hi << 4) | hi, (lo << 4) | lo);
            }
            break;
        case (G_IM_FMT_IA << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                u32 intensity = (texel >> 1) & 7;
                intensity = (intensity << 5) | (intensity << 2) | (intensity >> 1);
                out[i] = ia_to_rgba32(intensity, (texel & 1) ? 0xFF : 0x00);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_8b:
            for (i = 0; i < count; i++) {
                out[i] = ia_to_rgba32(src[i], src[i]);
            }
            break;
        case (G_IM_FMT_I << 2) | G_IM_SIZ_4b:
            for (i = 0; i < count; i++) {
                u32 texel = (i & 1) ? (src[i >> 1] & 0xF) : (src[i >> 1] >> 4);
                texel = (texel << 4) | texel;
                out[i] = ia_to_rgba32(texel, texel);
            }
            break;
        default:
            for (i = 0; i < count; i++) {
                out[i] = 0xFFFF00FF; // magenta: an unhandled format, so it's obvious
            }
            break;
    }
}

/**
 * The texture the current RDP state describes, decoded and uploaded once and
 * then kept. Returns 0 (untextured) if there is nothing sane to draw with.
 */
static u32 texture_lookup(void) {
    const u8 *tlut = (const u8 *) sTlutAddr;
    s32 i;

    if (sTexAddr == 0 || sTileWidth == 0 || sTileHeight == 0) {
        return 0;
    }
    if (sTileWidth * sTileHeight > GFX_MAX_TEX_TEXELS) {
        return 0;
    }
    if (sTileFmt == G_IM_FMT_CI) {
        if (sTlutAddr == 0) {
            return 0;
        }
        // CI4 palettes are 16 entries each, selected by the tile's palette field.
        if (sTileSiz == G_IM_SIZ_4b) {
            tlut += sTilePalette * 16 * 2;
        }
    }

    {
        u32 slot = tex_hint_slot(sTexAddr, (u32) tlut);
        s32 probe;

        // A few neighbouring hints, so two textures that hash alike do not push
        // each other out and send every lookup to the scan below.
        for (probe = 0; probe < GFX_TEX_HINT_PROBES; probe++) {
            s32 hi = (s32) sTexHint[(slot + probe) & (GFX_TEX_HINTS - 1)] - 1;

            if (hi >= 0 && hi < sTexCacheCount) {
                GfxTexture *t = &sTexCache[hi];

                if (t->timg == sTexAddr && t->tlut == (u32) tlut && t->fmt == sTileFmt &&
                    t->siz == sTileSiz && t->width == sTileWidth && t->height == sTileHeight &&
                    t->cmS == sTileCmS && t->cmT == sTileCmT && t->swapped == (u8) sTexSwapped) {
                    t->lastUsed = sGfxFrameCount;
                    return t->handle;
                }
            }
        }

        for (i = 0; i < sTexCacheCount; i++) {
            GfxTexture *t = &sTexCache[i];
            if (t->timg == sTexAddr && t->tlut == (u32) tlut && t->fmt == sTileFmt && t->siz == sTileSiz &&
                t->width == sTileWidth && t->height == sTileHeight && t->cmS == sTileCmS && t->cmT == sTileCmT &&
                t->swapped == (u8) sTexSwapped) {
                t->lastUsed = sGfxFrameCount;
                tex_hint_store(slot, i);
                return t->handle;
            }
        }
    }

    // A full cache used to mean drawing untextured forever. Evict the
    // least-recently-used entry instead; a texture still wanted is re-decoded next
    // time it's asked for. With invalidation working this shouldn't trigger, but a
    // slow frame beats a permanently untextured world.
    if (sTexCacheCount >= GFX_MAX_TEXTURES) {
        s32 oldest = 0;

        for (i = 1; i < sTexCacheCount; i++) {
            if (sTexCache[i].lastUsed < sTexCache[oldest].lastUsed) {
                oldest = i;
            }
        }
        if (!sTexCache[oldest].prebuilt) {
            gfx_delete_texture(sTexCache[oldest].handle);
        }
        sTexCache[oldest] = sTexCache[--sTexCacheCount];
    }

    {
        // Converted offline and already in video memory: no decoding.
        s32 p = (sTileFmt != G_IM_FMT_CI) ? prebuilt_find(sTexAddr) : -1;

        if (p >= 0 && sPrebuilt[p].width == sTileWidth && sPrebuilt[p].height == sTileHeight) {
            GfxTexture *t = &sTexCache[sTexCacheCount++];

            gfx_texture_set_wrap(sPrebuilt[p].handle, sTileCmS, sTileCmT);
            t->timg = sTexAddr;
            t->tlut = (u32) tlut;
            t->fmt = sTileFmt;
            t->siz = sTileSiz;
            t->swapped = (u8) sTexSwapped;
            t->width = sTileWidth;
            t->height = sTileHeight;
            t->cmS = sTileCmS;
            t->cmT = sTileCmT;
            t->lastUsed = sGfxFrameCount;
            t->handle = sPrebuilt[p].handle;
            t->prebuilt = 1;
            tex_hint_store(tex_hint_slot(t->timg, t->tlut), sTexCacheCount - 1);
            sTexPrebuiltHits++;
            return t->handle;
        }
    }
    sTexDecodes++;
#if DKR_TEX_TRACE
    {
        // Why this texture is not prebuilt: one line per texture.
        static u32 sTraced[256];
        static s32 sTracedCount;
        u32 key = sTexAddr ^ ((u32) tlut << 3) ^ ((u32) sTileWidth << 20) ^ ((u32) sTileHeight << 12);
        s32 k, p = prebuilt_find(sTexAddr);

        for (k = 0; k < sTracedCount && sTraced[k] != key; k++) {}
        if (k == sTracedCount && sTracedCount < 256) {
            sTraced[sTracedCount++] = key;
            printf("TEX DECODE timg=%08lx tlut=%08lx fmt=%d siz=%d tile=%dx%d swapped=%d prebuilt=%s%dx%d\n",
                   (unsigned long) sTexAddr, (unsigned long) tlut, sTileFmt, sTileSiz, sTileWidth, sTileHeight,
                   (int) sTexSwapped, p >= 0 ? "yes " : "no ", p >= 0 ? sPrebuilt[p].width : 0,
                   p >= 0 ? sPrebuilt[p].height : 0);
        }
    }
#endif

    {
        const u8 *texels = (const u8 *) sTexAddr;
        s32 rowBytes = row_bytes(sTileSiz, sTileWidth);

        // A dxt of 0 means the asset is stored pre-swizzled; put it back before
        // decoding. Guarded on the scratch buffer, which a sane texture never
        // exceeds — decoding the raw bytes is better than reading past it.
        if (sTexSwapped && (rowBytes * sTileHeight) <= (s32) sizeof(sTexSwizzleBuf)) {
            unswizzle_rows(texels, sTexSwizzleBuf, sTileSiz, rowBytes, sTileHeight);
            texels = sTexSwizzleBuf;
        }
        decode_texture(texels, sTileFmt, sTileSiz, sTileWidth, sTileHeight, tlut, sTexDecodeBuf);
    }

    {
        GfxTexture *t = &sTexCache[sTexCacheCount++];
        t->timg = sTexAddr;
        t->tlut = (u32) tlut;
        t->fmt = sTileFmt;
        t->siz = sTileSiz;
        t->swapped = (u8) sTexSwapped;
        t->width = sTileWidth;
        t->height = sTileHeight;
        t->cmS = sTileCmS;
        t->cmT = sTileCmT;
        t->lastUsed = sGfxFrameCount;
        t->handle = gfx_create_texture(sTexDecodeBuf, sTileWidth, sTileHeight, sTileCmS, sTileCmT);
        t->prebuilt = 0;
        tex_hint_store(tex_hint_slot(t->timg, t->tlut), sTexCacheCount - 1);
        return t->handle;
    }
}

// What texture_lookup() last returned, valid until the display list touches the
// tile or loads an image. Consecutive batches mostly share a texture, and the
// lookup is not free. A new frame starts dirty so that each texture's first use
// in a frame goes through the lookup, which is what stamps it as recently used.
static s32 sTexStateDirty = TRUE;
static u32 sTexCurrent;

static u32 texture_current(void) {
    if (sTexStateDirty) {
        sTexCurrent = texture_lookup();
        sTexStateDirty = FALSE;
    }
    return sTexCurrent;
}

/**
 * Perspective divide plus the viewport map — the last thing the RSP does before
 * handing a vertex to the RDP. Only ever called on clipped vertices, so w is
 * guaranteed positive here.
 */
static void combiner_eval(const f32 shade[4], u8 out[4], u8 sec[3]);
static void combiner_eval_lit(const f32 shade[4], u8 lit[4]);
static void combiner_classify(void);
static u8 clamp_u8(f32 v);

// How the current batch's combiner collapses, per channel; see
// combiner_classify(). sCcFast means every channel is either passthrough or a
// constant, so project() can skip the mux entirely.
static u8 sCcPass[4];  // 1 = this channel is the shade, unchanged
static u8 sCcConst[4]; // otherwise, the constant it always produces
static s32 sCcFast;

/**
 * The geometric half: perspective divide, viewport map and fog. Depends only on
 * RSP state (matrices, viewport, fog registers, geometry mode), all of which is
 * settled by the time the vertex is loaded — so this is what gets hoisted to
 * G_VTX time and cached in the GfxVertex.
 *
 * w is guaranteed >= GFX_NEAR_W by every caller, which is what makes the fsrra
 * reciprocal safe (it is defined for positive inputs only).
 */
static void project_geom(const GfxVertex *v, f32 *sx, f32 *sy, f32 *sz, f32 *sw, f32 *sfog) {
    f32 invW = shz_invf_fsrra(v->clip[3]);
    f32 ndcZ = v->clip[2] * invW;

    *sx = (v->clip[0] * invW * sVpScaleX) + sVpTransX;
    *sy = sVpTransY - (v->clip[1] * invW * sVpScaleY);
    // Negated so nearer geometry gets the smaller depth under GL_LESS.
    *sz = -ndcZ;
    // Kept so the host layer can restore the homogeneous position and get
    // perspective-correct texturing out of the fixed-function pipeline. The near
    // clip guarantees this is >= GFX_NEAR_W, so it is safe to multiply back by.
    *sw = v->clip[3];

    // Fog: the RSP's own formula, ndc_z * mul + ofs, clamped to a byte. The
    // geometry mode gates it — material_set() sets and clears G_FOG per material,
    // and clears it whenever a material wants vertex alpha instead, because the
    // RSP keeps the fog factor in the shade-alpha slot.
    if (sGeometryMode & G_FOG) {
        f32 fog = (ndcZ * (f32) sFogMul) + (f32) sFogOfs;

        if (fog < 0.0f) {
            fog = 0.0f;
        }
        if (fog > 255.0f) {
            fog = 255.0f;
        }
        *sfog = fog * (1.0f / 255.0f);
    } else {
        *sfog = 0.0f;
    }
}

/** Cache the projection into the vertex, or mark it as behind the near plane. */
static void project_into(GfxVertex *v) {
    if (v->clip[3] >= GFX_NEAR_W) {
        project_geom(v, &v->sx, &v->sy, &v->sz, &v->sw, &v->sfog);
    } else {
        v->sw = 0.0f; // behind the near plane — nothing cached
    }
}

/**
 * Turn a clip-space vertex plus its per-triangle UVs into an output vertex.
 * Takes the cached projection when there is one; only vertices manufactured by
 * clip_edge have to be projected here.
 *
 * The combiner is not hoisted with the geometry: it is RDP state, not RSP state,
 * so it can change between the G_VTX that loads a vertex and the G_TRIN that
 * draws with it.
 */
static void project(const GfxVertex *v, GfxTriVert *out) {
    if (VERTEX_PROJECTED(v)) {
        out->x = v->sx;
        out->y = v->sy;
        out->z = v->sz;
        out->w = v->sw;
        out->fog = v->sfog;
    } else {
        project_geom(v, &out->x, &out->y, &out->z, &out->w, &out->fog);
    }
    out->u = v->u;
    out->v = v->v;

    // The colour the RDP would compute around the texel: the environment blend,
    // the prim colour, and the prim/vertex alpha behind every fade. The texture
    // unit modulates the real texel in afterwards, so the combiner runs here with
    // the texel as white. Which path applies was settled per batch; see
    // combiner_classify().
    if (sCcFast) {
        // Every channel is the shade straight through or a per-batch constant, so
        // the mux collapses to four byte selects, no FP. (The blast path makes
        // the same call on the same classification.)
        out->r = sCcPass[0] ? v->r : sCcConst[0];
        out->g = sCcPass[1] ? v->g : sCcConst[1];
        out->b = sCcPass[2] ? v->b : sCcConst[2];
        out->a = sCcPass[3] ? v->a : sCcConst[3];
    } else {
        // At least one channel mixes shade with something else; run the real mux.
        // Only the lit half is wanted, see combiner_eval_lit.
        f32 shade[4];
        u8 lit[4];

        shade[0] = v->r * (1.0f / 255.0f);
        shade[1] = v->g * (1.0f / 255.0f);
        shade[2] = v->b * (1.0f / 255.0f);
        shade[3] = v->a * (1.0f / 255.0f);
        combiner_eval_lit(shade, lit);

        out->r = lit[0];
        out->g = lit[1];
        out->b = lit[2];
        out->a = lit[3];
    }
}

/**
 * Project a triangle and, unless it is marked double-sided, drop it if it is
 * facing away. Winding only exists after the perspective divide, which is why
 * this can't happen back in clip space.
 */
static void push_tri(const GfxVertex *a, const GfxVertex *b, const GfxVertex *c, s32 cull) {
    // Project straight into the output buffer; a culled triangle just doesn't
    // advance the count. Saves the 96-byte local + copy per surviving triangle.
    GfxTriVert *v = &sTriVerts[sTriVertCount];
    f32 area;

    if (sTriVertCount + 3 > GFX_MAX_TRI_VERTS) {
        return;
    }

    project(a, &v[0]);
    project(b, &v[1]);
    project(c, &v[2]);

    if (cull) {
        area = ((v[1].x - v[0].x) * (v[2].y - v[0].y)) - ((v[2].x - v[0].x) * (v[1].y - v[0].y));
        if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
            return;
        }
    }

    sTriVertCount += 3;
}

/**
 * Split the edge a->b where it crosses the near plane, at the point where
 * w == GFX_NEAR_W. Colour interpolates linearly in clip space along with the
 * position, which is what the RSP's own clipper does.
 */
static void clip_edge(const GfxVertex *a, const GfxVertex *b, GfxVertex *out) {
    f32 t = shz_divf(GFX_NEAR_W - a->clip[3], b->clip[3] - a->clip[3]);
    s32 i;

    for (i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + ((b->clip[i] - a->clip[i]) * t);
    }
    out->u = a->u + ((b->u - a->u) * t);
    out->v = a->v + ((b->v - a->v) * t);
    out->r = (u8) (a->r + ((f32) (b->r - a->r) * t));
    out->g = (u8) (a->g + ((f32) (b->g - a->g) * t));
    out->b = (u8) (a->b + ((f32) (b->b - a->b) * t));
    out->a = (u8) (a->a + ((f32) (b->a - a->a) * t));

    // Brand new clip-space position — nothing cached applies to it. project()
    // will compute the projection for this one on the spot.
    out->sw = 0.0f;
}

/**
 * Clip a triangle against the near plane (Sutherland-Hodgman on the single
 * w >= GFX_NEAR_W plane), then fan-triangulate whatever polygon survives and
 * emit it. One clipped triangle yields 0, 1 or 2 output triangles.
 */
static void emit_triangle(const GfxVertex *v0, const GfxVertex *v1, const GfxVertex *v2, s32 cull) {
    const GfxVertex *in[3];
    GfxVertex poly[4];
    s32 numOut = 0;
    s32 i;

    // Nothing crosses the near plane, which is the overwhelmingly common case:
    // emit straight from the loaded vertices instead of staging the triangle
    // through poly[]. A cached projection exists exactly when
    // clip[3] >= GFX_NEAR_W, so this is the same test the loop below makes.
    if (VERTEX_PROJECTED(v0) && VERTEX_PROJECTED(v1) && VERTEX_PROJECTED(v2)) {
        push_tri(v0, v1, v2, cull);
        return;
    }

    in[0] = v0;
    in[1] = v1;
    in[2] = v2;

    for (i = 0; i < 3; i++) {
        const GfxVertex *cur = in[i];
        const GfxVertex *next = in[(i + 1) % 3];
        s32 curIn = cur->clip[3] >= GFX_NEAR_W;
        s32 nextIn = next->clip[3] >= GFX_NEAR_W;

        if (curIn) {
            poly[numOut++] = *cur;
        }
        if (curIn != nextIn) {
            clip_edge(cur, next, &poly[numOut++]);
        }
    }

    if (numOut < 3) {
        return; // entirely behind the near plane
    }

    for (i = 2; i < numOut; i++) {
        push_tri(&poly[0], &poly[i - 1], &poly[i], cull);
    }
}


static void apply_texture_filter(void) {
    u32 filt = sOtherModeH & (3 << G_MDSFT_TEXTFILT);

    gfx_set_texture_filter(filt == G_TF_POINT);
}

static void apply_render_mode(void) {
    u32 alphaCompare = sOtherModeL & (3 << G_MDSFT_ALPHACOMPARE);
    s32 zEnabled = (sGeometryMode & G_ZBUFFER) != 0;
    f32 ref = 0.0f;

    // Depth needs both halves to agree: the RSP has to be emitting z (geometry
    // mode) and the RDP has to be told to use it (render mode). Clearing G_ZBUFFER
    // is how the game turns depth off for the sky, overlays and its 2D layer.
    gfx_set_depth_test(zEnabled && (sOtherModeL & Z_CMP) != 0);
    gfx_set_depth_write(zEnabled && (sOtherModeL & Z_UPD) != 0);
    gfx_set_depth_offset(zEnabled && (sOtherModeL & ZMODE_DEC) == ZMODE_DEC);
    {
        // FORCE_BL only says the blender runs. It blends with what is already
        // drawn only if its equation reads the framebuffer; G_RM_OPA_SURF, the
        // sky's mode, forces the blender and still outputs the pixel unchanged.
        // The equation is P*A + M*B, two bits each, with one set per cycle; the
        // last cycle's set decides.
        s32 twoCycle = (sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
        u32 p = (sOtherModeL >> (twoCycle ? 28 : 30)) & 3;
        u32 m = (sOtherModeL >> (twoCycle ? 20 : 22)) & 3;

        gfx_set_blend((sOtherModeL & FORCE_BL) != 0 && (p == G_BL_CLR_MEM || m == G_BL_CLR_MEM));
    }

    // CVG_X_ALPHA is how the cutout materials (G_RM_*_TEX_EDGE) get their hard
    // edge: the RDP multiplies coverage by alpha, which on a non-antialiased
    // host is a straight 50% cutout. Otherwise a threshold compare comes from
    // the blend colour's alpha, and G_AC_DITHER — which DKR only uses where a
    // dithered edge is cosmetic — degrades to the same "drop the invisible
    // texels" default as G_AC_NONE.
    if (sOtherModeL & CVG_X_ALPHA) {
        ref = 0.5f;
    } else if (alphaCompare == G_AC_THRESHOLD) {
        ref = sBlendColor[3] / 255.0f;
    }
    gfx_set_alpha_test(ref);
}

/**
 * G_TRIN — DKR's polygon command. w1 points at an array of Triangles (vertex
 * indices plus UVs); shade each one from its vertex colours.
 */
static s32 polygon_blast(const Triangle *tris, s32 count, s32 texEnabled);

static void handle_polygon(u32 w0, u32 w1) {
    const Triangle *tris = (const Triangle *) w1;
    u32 params = (w0 >> 16) & 0xFF;
    s32 count = (params >> 4) + 1;
    s32 texEnabled = params & 1;
    u32 texture = 0;
    f32 invTexW = 0.0f, invTexH = 0.0f;
    s32 i;

    if (tris == NULL) {
        return;
    }
    if (polygon_blast(tris, count, texEnabled)) {
        return;
    }

    PROF_LAP_START();
    if (texEnabled) {
        texture = texture_current();
    }
    if (texture != 0) {
        // UVs are S10.5 texel coordinates (32 = one texel), so normalising is a
        // divide by 32 and then by the texture's size.

        invTexW = shz_invf_fsrra(32.0f * (f32) sTileWidth);
        invTexH = shz_invf_fsrra(32.0f * (f32) sTileHeight);
    }

    // Each G_TRIN is one material batch, so it becomes one draw call.
    sTriVertCount = 0;

    // Settle how this batch's combiner collapses before touching any vertex. The
    // mux, prim and env cannot change inside a batch, so once is enough.
    combiner_classify();
    PROF_LAP(tPolyState);

    for (i = 0; i < count; i++) {
        // The UVs are the only per-triangle part of a vertex, so write them into
        // the loaded vertices in place and pass those along, rather than taking a
        // copy of each GfxVertex per corner. The copy only existed to carry the
        // UVs, and it costs more now that the struct also caches the projection.
        // Overwriting is safe because emit_triangle consumes them before the next
        // iteration touches them again.
        GfxVertex *a = &sVerts[tris[i].vi0 % GFX_MAX_VERTS];
        GfxVertex *b = &sVerts[tris[i].vi1 % GFX_MAX_VERTS];
        GfxVertex *c = &sVerts[tris[i].vi2 % GFX_MAX_VERTS];

        a->u = tris[i].uv0.u * invTexW;
        a->v = tris[i].uv0.v * invTexH;
        b->u = tris[i].uv1.u * invTexW;
        b->v = tris[i].uv1.v * invTexH;
        c->u = tris[i].uv2.u * invTexW;
        c->v = tris[i].uv2.v * invTexH;

        // DKR marks culling per triangle: BACKFACE_DRAW means double-sided.
        emit_triangle(a, b, c, !(tris[i].flags & BACKFACE_DRAW));
    }

    PROF_LAP(tPolyEmit);
    apply_render_mode();
    gfx_set_fog((sGeometryMode & G_FOG) != 0, sFogColor);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_set_texenv_modulate(); // the 2D path leaves the env in blend mode
    PROF_ADD(slowTris, sTriVertCount / 3);
    PROF_ADD(sentFrame, sTriVertCount / 3);
    PROF_ADD(slowBatches, 1);
    gfx_draw_tris(sTriVerts, sTriVertCount);
    PROF_LAP(tPolyDraw);
}

// ---------------------------------------------------------------------------
// The blast path — native rendering for simple G_VTX + G_TRIN batches.
//
// run_dl fuses any G_VTX immediately followed by the one G_TRIN that consumes it
// (see the G_VTX case) into blast_render. The DL has already run the batch's
// material (matrices, tile state, render mode, combiner), so this reads the same
// interpreter state the emulated path would. It skips the emulated path's
// per-batch machinery: no sVerts staging, no per-corner combiner FP, no
// GfxTriVert copies, no sTriVerts round-trip — one transform per vertex and a
// store-queue write.
//
// Fallback: if the combiner doesn't collapse to select/passthrough, if
// billboarding is on, or if the gfx layer needs a path blast doesn't cover, the
// batch runs the exact G_VTX/G_TRIN handlers the fusion replaced. Vertices behind
// the near plane don't fall back: their triangles go through an in-path clip
// (blast_clip_tri).
// ---------------------------------------------------------------------------

// The asset can't exceed these: gSPVertexDKR encodes count-1 in 5 bits,
// gSPPolygon in 4.
#define GFX_BLAST_MAX_VERTS 32
#define GFX_BLAST_MAX_TRIS 16


/** Render a blast batch through the emulated path, as the DL would have. */
static void blast_fallback(const Vertex *verts, s32 numVerts, const Vertex *verts2, s32 numVerts2, s32 matrix2,
                           const Triangle *tris, s32 numTris, s32 texEnabled) {
    PROF_ADD(fallback, 1);
#if DKR_GFX_PROFILE_SKIP == 3
    PROF_ADD(slowTris, numTris);
    return;
#endif
    PROF_TIME(tFallback, handle_vertex(((u32) G_VTX << 24) | ((u32) ((numVerts - 1) << 3) << 16), (u32) verts));
    if (numVerts2 > 0) {
        // The second run was loaded under another matrix and appended.
        s32 matrix = sCurMatrix;

        sCurMatrix = matrix2;
        PROF_TIME(tFallback, handle_vertex(((u32) G_VTX << 24) |
                                               ((u32) (((numVerts2 - 1) << 3) | G_VTX_APPEND) << 16),
                                           (u32) verts2));
        sCurMatrix = matrix;
    }
    PROF_TIME(tFallback, handle_polygon(((u32) G_TRIN << 24) | ((u32) (((numTris - 1) << 4) | texEnabled) << 16),
                                        (u32) tris));
}

// N64-pixels -> framebuffer scale, from gfx_blast_begin. File-scope so the
// cold clip path shares the hot loop's emit.
static f32 sBlastSX = 1.0f, sBlastSY = 1.0f;

// Where a blended batch's vertices are recorded; NULL streams to the open list.
static pvr_vertex_t *sBlastRecord;
// Depth = 1/w * mul + add: 1 and 0, but decals and far backdrops differ (gfx_blast_begin).
static f32 sBlastZMul = 1.0f, sBlastZAdd = 0.0f;

/** One PVR vertex to the store queue or the recording — inline, no cross-file call. */
static inline void blast_emit(f32 x, f32 y, f32 invw, f32 u, f32 v, u32 color, u32 flags) {
    pvr_vertex_t *vt = sBlastRecord ? sBlastRecord : (pvr_vertex_t *) pvr_dr_target();

    vt->flags = flags;
    vt->x = x * sBlastSX;
    vt->y = y * sBlastSY;
    vt->z = (invw * sBlastZMul) + sBlastZAdd;
    vt->u = u;
    vt->v = v;
    vt->argb = color;
    vt->oargb = 0;
    if (sBlastRecord) {
        sBlastRecord++;
    } else {
        pvr_dr_commit(vt);
    }
}

// A corner in clip space on its way through the blast near-clipper: position,
// pre-scaled UVs and the batch-resolved colour.
typedef struct {
    shz_vec4_t c;
    f32 u, v;
    u32 argb;
} BlastClipVert;

static u32 blast_argb_lerp(u32 a, u32 b, f32 t) {
    u32 out = 0;
    s32 s;

    for (s = 0; s < 32; s += 8) {
        f32 ca = (f32) ((a >> s) & 0xFF);
        f32 cb = (f32) ((b >> s) & 0xFF);

        out |= ((u32) (u8) (ca + ((cb - ca) * t))) << s;
    }
    return out;
}

/**
 * Near-clip one triangle in clip space (Sutherland-Hodgman against
 * w >= GFX_NEAR_W, same plane and interpolation as the emulated clipper),
 * project the survivors and emit the fan. Only triangles that actually cross
 * the plane come here, so this is cold.
 */
static void blast_clip_tri(const BlastClipVert *in, s32 cull) {
    BlastClipVert poly[4];
    f32 px[4], py[4], iw[4];
    s32 n = 0;
    s32 j;
    f32 area;

    for (j = 0; j < 3; j++) {
        const BlastClipVert *a = &in[j];
        const BlastClipVert *b = &in[(j + 1) % 3];
        s32 ina = a->c.w >= GFX_NEAR_W;
        s32 inb = b->c.w >= GFX_NEAR_W;

        if (ina) {
            poly[n++] = *a;
        }
        if (ina != inb) {
            f32 t = shz_divf(GFX_NEAR_W - a->c.w, b->c.w - a->c.w);
            BlastClipVert *o = &poly[n++];

            o->c.x = a->c.x + ((b->c.x - a->c.x) * t);
            o->c.y = a->c.y + ((b->c.y - a->c.y) * t);
            o->c.z = a->c.z + ((b->c.z - a->c.z) * t);
            o->c.w = a->c.w + ((b->c.w - a->c.w) * t);
            o->u = a->u + ((b->u - a->u) * t);
            o->v = a->v + ((b->v - a->v) * t);
            o->argb = blast_argb_lerp(a->argb, b->argb, t);
        }
    }
    if (n < 3) {
        return;
    }
    for (j = 0; j < n; j++) {
        f32 invW = shz_invf_fsrra(poly[j].c.w);

        px[j] = (poly[j].c.x * invW * sVpScaleX) + sVpTransX;
        py[j] = sVpTransY - (poly[j].c.y * invW * sVpScaleY);
        iw[j] = invW;
    }
    if (cull) {
        area = ((px[1] - px[0]) * (py[2] - py[0])) - ((px[2] - px[0]) * (py[1] - py[0]));
        if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
            return;
        }
    }
    for (j = 1; j + 1 < n; j++) {
        blast_emit(px[0], py[0], iw[0], poly[0].u, poly[0].v, poly[0].argb, PVR_CMD_VERTEX);
        blast_emit(px[j], py[j], iw[j], poly[j].u, poly[j].v, poly[j].argb, PVR_CMD_VERTEX);
        blast_emit(px[j + 1], py[j + 1], iw[j + 1], poly[j + 1].u, poly[j + 1].v,
                   poly[j + 1].argb, PVR_CMD_VERTEX_EOL);
    }
}

#if DKR_GFX_PROFILE
static void prof_slow_cc(u32 verts) {
    u32 cyc = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    u32 prim = (sPrimColor[0] << 24) | (sPrimColor[1] << 16) | (sPrimColor[2] << 8) | sPrimColor[3];
    u32 env = (sEnvColor[0] << 24) | (sEnvColor[1] << 16) | (sEnvColor[2] << 8) | sEnvColor[3];
    s32 i;

    for (i = 0; i < PROF_SLOW_CC; i++) {
        if (sSlowCc[i].verts == 0 || (sSlowCc[i].w0 == sCombineW0 && sSlowCc[i].w1 == sCombineW1 &&
                                      sSlowCc[i].cyc == cyc && sSlowCc[i].prim == prim && sSlowCc[i].env == env)) {
            sSlowCc[i].w0 = sCombineW0;
            sSlowCc[i].w1 = sCombineW1;
            sSlowCc[i].cyc = cyc;
            sSlowCc[i].prim = prim;
            sSlowCc[i].env = env;
            sSlowCc[i].verts += verts;
            return;
        }
    }
}
#endif

static void blast_render(const Vertex *verts, s32 numVerts, const Vertex *verts2, s32 numVerts2, s32 matrix2,
                         const Triangle *tris, s32 numTris, s32 texEnabled) {
    f32 sx[GFX_BLAST_MAX_VERTS], sy[GFX_BLAST_MAX_VERTS], iw[GFX_BLAST_MAX_VERTS];
    u32 argb[GFX_BLAST_MAX_VERTS];
    shz_vec4_t cp[GFX_BLAST_MAX_VERTS];
    u32 behind = 0;
    u32 texture = 0;
    f32 uMul = 0.0f, vMul = 0.0f;
    f32 uS, vS;
    s32 i;
    s32 total = numVerts + numVerts2;

    if (verts == NULL || tris == NULL || numTris <= 0 || numVerts <= 0) {
        return;
    }
    if (total > GFX_BLAST_MAX_VERTS || numTris > GFX_BLAST_MAX_TRIS) {
        // Can't occur in a valid asset (the DL macros can't encode it).
        printf("blast: oversized batch %dv/%dt\n", (int) numVerts, (int) numTris);
        return;
    }

    PROF_LAP_START();
    // The color must be per-batch trivial: shade byte or constant per channel.
    combiner_classify();
    if (sBillboard) {
        PROF_ADD(whyBillboard, numTris);
        blast_fallback(verts, numVerts, verts2, numVerts2, matrix2, tris, numTris, texEnabled);
        return;
    }

    // Resolve this batch's state before checking gfx_blast_viable(), which reads
    // it: until apply_render_mode runs it's still the previous batch's (a stale
    // alpha test sent good batches to the slow path).
    if (texEnabled) {
        texture = texture_current();
    }
    apply_render_mode();
    gfx_set_fog((sGeometryMode & G_FOG) != 0, sFogColor);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_set_texenv_modulate();

    // States begin() would refuse are known now, so don't transform first. The
    // fallback's handle_polygon redoes this state, so these state calls are safe
    // to have run.
    if (!gfx_blast_viable()) {
        PROF_ADD(whyState, numTris);
#if DKR_GFX_PROFILE
        {
            extern int gfx_blast_why(void);
            sProf.whyStateKind[gfx_blast_why()] += numTris;
        }
#endif
        blast_fallback(verts, numVerts, verts2, numVerts2, matrix2, tris, numTris, texEnabled);
        return;
    }

    PROF_ADD(blast, 1);
    PROF_ADD(verts, total);
#if DKR_GFX_PROFILE
    if (!sCcFast) {
        prof_slow_cc((u32) total);
    }
#endif
    PROF_ADD(tris, numTris);
    PROF_LAP(tState);
#if DKR_GFX_PROFILE_SKIP == 2
    return;
#endif

    // Transform + project every vertex. A vertex behind the near plane just marks
    // its bit; the triangles touching it go through the in-path clipper below.
    {
        // Each channel is the vertex's own or a per-batch constant; settle which
        // once, so a vertex costs a mask and an or.
        u32 keep = (sCcPass[3] ? 0xFF000000u : 0) | (sCcPass[0] ? 0x00FF0000u : 0) |
                   (sCcPass[1] ? 0x0000FF00u : 0) | (sCcPass[2] ? 0x000000FFu : 0);
        u32 konst = (((u32) sCcConst[3] << 24) | ((u32) sCcConst[0] << 16) | ((u32) sCcConst[1] << 8) |
                     (u32) sCcConst[2]) & ~keep;
        f32 vpSX = sVpScaleX, vpSY = sVpScaleY, vpTX = sVpTransX, vpTY = sVpTransY;
        s32 ccFast = sCcFast;
        s32 run;

        i = 0;
        for (run = 0; run < 2; run++) {
            const Vertex *v = run ? verts2 : verts;
            const Vertex *end = v + (run ? numVerts2 : numVerts);

            if (v == end) {
                continue;
            }
            shz_xmtrx_load_4x4((shz_mat4x4_t *) &sMatrices[run ? matrix2 : sCurMatrix]);
            for (; v < end; v++, i++) {
                shz_vec4_t c = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
                f32 invW;

                cp[i] = c;
                if (ccFast) {
                    argb[i] = ((((u32) v->a << 24) | ((u32) v->r << 16) | ((u32) v->g << 8) | (u32) v->b) & keep) |
                              konst;
                } else {
                    // The combiner mixes shade with something else: run the real
                    // mux, once per vertex rather than once per corner.
                    f32 shade[4];
                    u8 lit[4];

                    shade[0] = v->r * (1.0f / 255.0f);
                    shade[1] = v->g * (1.0f / 255.0f);
                    shade[2] = v->b * (1.0f / 255.0f);
                    shade[3] = v->a * (1.0f / 255.0f);
                    combiner_eval_lit(shade, lit);
                    argb[i] = ((u32) lit[3] << 24) | ((u32) lit[0] << 16) | ((u32) lit[1] << 8) | (u32) lit[2];
                }
                if (c.w < GFX_NEAR_W) {
                    behind |= 1u << i;
                    continue;
                }
                invW = shz_invf_fsrra(c.w);
                sx[i] = (c.x * invW * vpSX) + vpTX;
                sy[i] = vpTY - (c.y * invW * vpSY);
                iw[i] = invW;
            }
        }
    }

    PROF_LAP(tXform);
#if DKR_GFX_PROFILE
    if (sOtherModeL & FORCE_BL) {
        extern int gfx_texture_kind(void);
        u32 vtxAnd = 0xFF, outAnd = 0xFF000000u;

        for (i = 0; i < numVerts; i++) {
            vtxAnd &= verts[i].a;
            outAnd &= argb[i];
        }
        prof_blend(sOtherModeL, sCombineW0, sCombineW1, gfx_texture_kind(), vtxAnd == 0xFF, outAnd == 0xFF000000u,
                   (sGeometryMode & G_FOG) != 0, sPrimColor[3], sEnvColor[3], numTris);
    }
#endif
    // A triangle crossing the near plane can come back as two.
    if (!gfx_blast_begin(&uS, &vS, &sBlastSX, &sBlastSY, numTris * 6, (void **) &sBlastRecord,
                         (int) (argb[0] >> 24), &sBlastZMul, &sBlastZAdd)) {
        // handle_polygon redoes this state, so falling back here is safe.
        PROF_ADD(whyFull, numTris);
        blast_fallback(verts, numVerts, verts2, numVerts2, matrix2, tris, numTris, texEnabled);
        return;
    }
    if (texture != 0) {
        uMul = shz_invf_fsrra(32.0f * (f32) sTileWidth) * uS;
        vMul = shz_invf_fsrra(32.0f * (f32) sTileHeight) * vS;
    }

    for (i = 0; i < numTris; i++) {
        const Triangle *t = &tris[i];
        s32 i0 = t->vi0, i1 = t->vi1, i2 = t->vi2;

        if (i0 >= total || i1 >= total || i2 >= total) {
            continue;
        }
        if (behind & ((1u << i0) | (1u << i1) | (1u << i2))) {
            BlastClipVert cv[3];

            cv[0].c = cp[i0];
            cv[0].u = t->uv0.u * uMul;
            cv[0].v = t->uv0.v * vMul;
            cv[0].argb = argb[i0];
            cv[1].c = cp[i1];
            cv[1].u = t->uv1.u * uMul;
            cv[1].v = t->uv1.v * vMul;
            cv[1].argb = argb[i1];
            cv[2].c = cp[i2];
            cv[2].u = t->uv2.u * uMul;
            cv[2].v = t->uv2.v * vMul;
            cv[2].argb = argb[i2];
            PROF_ADD(clipped, 1);
#if DKR_GFX_PROFILE_SKIP != 1
            blast_clip_tri(cv, !(t->flags & BACKFACE_DRAW));
#endif
            continue;
        }
        if (!(t->flags & BACKFACE_DRAW)) {
            f32 area = ((sx[i1] - sx[i0]) * (sy[i2] - sy[i0])) -
                       ((sx[i2] - sx[i0]) * (sy[i1] - sy[i0]));
            if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
                PROF_ADD(culled, 1);
                continue;
            }
        }
        PROF_ADD(sent, 1);
        PROF_ADD(sentFrame, 1);
#if DKR_GFX_PROFILE_SKIP == 1
        continue;
#endif
        blast_emit(sx[i0], sy[i0], iw[i0], t->uv0.u * uMul, t->uv0.v * vMul, argb[i0],
                   PVR_CMD_VERTEX);
        blast_emit(sx[i1], sy[i1], iw[i1], t->uv1.u * uMul, t->uv1.v * vMul, argb[i1],
                   PVR_CMD_VERTEX);
        blast_emit(sx[i2], sy[i2], iw[i2], t->uv2.u * uMul, t->uv2.v * vMul, argb[i2],
                   PVR_CMD_VERTEX_EOL);
    }
    if (sBlastRecord) {
        gfx_blast_end(sBlastRecord);
        sBlastRecord = NULL;
    }
    PROF_LAP(tEmit);
}

/**
 * A G_TRIN over vertices that G_VTX already loaded and projected: sprites, whose
 * quads share one appended load across several textures, and any batch the
 * peephole could not fuse. Writes them straight out like the blast path when
 * the state allows and no corner is behind the near plane; returns FALSE to
 * leave the batch to the emulated path otherwise.
 */
static s32 polygon_blast(const Triangle *tris, s32 count, s32 texEnabled) {
    u32 texture = 0;
    f32 uMul = 0.0f, vMul = 0.0f;
    f32 uS, vS;
    u32 keep, konst;
    s32 i;

    for (i = 0; i < count; i++) {
        if (!VERTEX_PROJECTED(&sVerts[tris[i].vi0 % GFX_MAX_VERTS]) ||
            !VERTEX_PROJECTED(&sVerts[tris[i].vi1 % GFX_MAX_VERTS]) ||
            !VERTEX_PROJECTED(&sVerts[tris[i].vi2 % GFX_MAX_VERTS])) {
            return FALSE;
        }
    }
    combiner_classify();
    if (!sCcFast) {
        return FALSE;
    }
    if (texEnabled) {
        texture = texture_current();
    }
    apply_render_mode();
    gfx_set_fog((sGeometryMode & G_FOG) != 0, sFogColor);
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_set_texenv_modulate();
    keep = (sCcPass[3] ? 0xFF000000u : 0) | (sCcPass[0] ? 0x00FF0000u : 0) | (sCcPass[1] ? 0x0000FF00u : 0) |
           (sCcPass[2] ? 0x000000FFu : 0);
    konst = (((u32) sCcConst[3] << 24) | ((u32) sCcConst[0] << 16) | ((u32) sCcConst[1] << 8) | (u32) sCcConst[2]) &
            ~keep;
    if (!gfx_blast_begin(&uS, &vS, &sBlastSX, &sBlastSY, count * 3, (void **) &sBlastRecord,
                         (int) (((((u32) sVerts[tris[0].vi0 % GFX_MAX_VERTS].a << 24) & keep) | konst) >> 24),
                         &sBlastZMul, &sBlastZAdd)) {
        return FALSE;
    }
    if (texture != 0) {
        uMul = shz_invf_fsrra(32.0f * (f32) sTileWidth) * uS;
        vMul = shz_invf_fsrra(32.0f * (f32) sTileHeight) * vS;
    }
    PROF_ADD(tris, count);
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        const GfxVertex *a = &sVerts[t->vi0 % GFX_MAX_VERTS];
        const GfxVertex *b = &sVerts[t->vi1 % GFX_MAX_VERTS];
        const GfxVertex *c = &sVerts[t->vi2 % GFX_MAX_VERTS];

        if (!(t->flags & BACKFACE_DRAW)) {
            f32 area = ((b->sx - a->sx) * (c->sy - a->sy)) - ((c->sx - a->sx) * (b->sy - a->sy));

            if (area * GFX_FRONT_FACE_SIGN <= 0.0f) {
                PROF_ADD(culled, 1);
                continue;
            }
        }
        PROF_ADD(sent, 1);
        PROF_ADD(sentFrame, 1);
        blast_emit(a->sx, a->sy, shz_invf_fsrra(a->sw), t->uv0.u * uMul, t->uv0.v * vMul,
                   ((((u32) a->a << 24) | ((u32) a->r << 16) | ((u32) a->g << 8) | (u32) a->b) & keep) | konst,
                   PVR_CMD_VERTEX);
        blast_emit(b->sx, b->sy, shz_invf_fsrra(b->sw), t->uv1.u * uMul, t->uv1.v * vMul,
                   ((((u32) b->a << 24) | ((u32) b->r << 16) | ((u32) b->g << 8) | (u32) b->b) & keep) | konst,
                   PVR_CMD_VERTEX);
        blast_emit(c->sx, c->sy, shz_invf_fsrra(c->sw), t->uv2.u * uMul, t->uv2.v * vMul,
                   ((((u32) c->a << 24) | ((u32) c->r << 16) | ((u32) c->g << 8) | (u32) c->b) & keep) | konst,
                   PVR_CMD_VERTEX_EOL);
    }
    if (sBlastRecord) {
        gfx_blast_end(sBlastRecord);
        sBlastRecord = NULL;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// The colour combiner.
//
// Evaluated on the CPU per vertex, with TEXEL0/TEXEL1 taken as white; the GL
// texture unit modulates the real texel in afterwards. This is exact for any
// combiner that reduces to "texel times a constant" and approximate for the rest
// (the RDP multiplies the texel only into the terms that name it; this multiplies
// it into the whole result). Needs no shader, GL_COMBINE or multitexture, like
// the OoT DC port.
//
// SHADE is a real input here. DKR's directionally-lit materials
// (dRenderSettingsDirectionalLighting, via directional_lighting_on() — the intro
// cutscene, for one) are
//
//     cycle 1: G_CC_BLEND_SHADEALPHA  ->  lerp(PRIM, TEXEL0, SHADE_ALPHA)
//     cycle 2: G_CC_BLENDI_SHADE      ->  lerp(COMBINED, ENV, SHADE)
//
// where the vertex colour is a blend weight and the lit colour comes from PRIM
// and ENV. Shading those as texel x vertex-colour collapses the model towards
// black (Diddy's plane went dark).
// ---------------------------------------------------------------------------

/**
 * A colour input from one of the 4-bit (a, b) or 3-bit (d) slots. The 4-bit slots
 * encode 0..7 and treat anything from 8 up as zero; the 3-bit d slot puts 1 at 6
 * and zero at 7. Slot 7 of a 4-bit slot is NOISE/K4, neither of which DKR uses,
 * so it lands in the zero default.
 */
static f32 sCcTexel = 1.0f; // the value TEXEL0/TEXEL1 take for this evaluation

static void cc_rgb_in(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    s32 i;

    switch (mux) {
        case G_CCMUX_COMBINED:
            for (i = 0; i < 3; i++) {
                out[i] = comb[i];
            }
            return;
        case G_CCMUX_TEXEL0:
        case G_CCMUX_TEXEL1: // no TEXEL1 yet; it follows TEXEL0
            out[0] = out[1] = out[2] = sCcTexel;
            return;
        case G_CCMUX_1:
            out[0] = out[1] = out[2] = 1.0f;
            return;
        case G_CCMUX_PRIMITIVE:
            for (i = 0; i < 3; i++) {
                out[i] = sPrimColor[i] / 255.0f;
            }
            return;
        case G_CCMUX_SHADE:
            for (i = 0; i < 3; i++) {
                out[i] = shade[i];
            }
            return;
        case G_CCMUX_ENVIRONMENT:
            for (i = 0; i < 3; i++) {
                out[i] = sEnvColor[i] / 255.0f;
            }
            return;
        default:
            out[0] = out[1] = out[2] = 0.0f;
            return;
    }
}

/**
 * A colour input from the 5-bit multiplier (c) slot, which also reaches the alpha
 * registers: the source of ENV_ALPHA and SHADE_ALPHA, both used by DKR's lighting.
 */
static void cc_rgb_mul(u32 mux, const f32 shade[4], const f32 comb[4], f32 out[3]) {
    f32 scalar;

    switch (mux) {
        case G_CCMUX_COMBINED_ALPHA:
            scalar = comb[3];
            break;
        case G_CCMUX_TEXEL0_ALPHA:
        case G_CCMUX_TEXEL1_ALPHA:
            scalar = sCcTexel;
            break;
        case G_CCMUX_PRIMITIVE_ALPHA:
            scalar = sPrimColor[3] / 255.0f;
            break;
        case G_CCMUX_SHADE_ALPHA:
            scalar = shade[3];
            break;
        case G_CCMUX_ENV_ALPHA:
            scalar = sEnvColor[3] / 255.0f;
            break;
        default:
            // Everything below 7 is a plain colour input; LOD_FRAC and K5 are not
            // used by DKR and fall through cc_rgb_in()'s zero default.
            cc_rgb_in(mux, shade, comb, out);
            return;
    }
    out[0] = out[1] = out[2] = scalar;
}

/** Every alpha slot is 3 bits and they all share one encoding. */
static f32 cc_alpha_in(u32 mux, const f32 shade[4], const f32 comb[4]) {
    switch (mux) {
        case G_ACMUX_COMBINED:
            return comb[3];
        case G_ACMUX_TEXEL0:
        case G_ACMUX_TEXEL1:
            return sCcTexel;
        case G_ACMUX_1:
            return 1.0f;
        case G_ACMUX_PRIMITIVE:
            return sPrimColor[3] / 255.0f;
        case G_ACMUX_SHADE:
            return shade[3];
        case G_ACMUX_ENVIRONMENT:
            return sEnvColor[3] / 255.0f;
        default:
            return 0.0f; // G_ACMUX_0
    }
}

static f32 cc_clamp(f32 v) {
    if (v <= 0.0f) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

static u8 clamp_u8(f32 v) {
    return (u8) (cc_clamp(v) * 255.0f);
}

/** One cycle of (a - b) * c + d, colour and alpha on their own muxes. */
static void cc_cycle(u32 a, u32 b, u32 c, u32 d, u32 aA, u32 bA, u32 cA, u32 dA, const f32 shade[4], const f32 in[4],
                     f32 out[4]) {
    f32 va[3], vb[3], vc[3], vd[3];
    s32 i;

    cc_rgb_in(a, shade, in, va);
    cc_rgb_in(b, shade, in, vb);
    cc_rgb_mul(c, shade, in, vc);
    cc_rgb_in(d, shade, in, vd);

    for (i = 0; i < 3; i++) {
        out[i] = cc_clamp(((va[i] - vb[i]) * vc[i]) + vd[i]);
    }
    out[3] = cc_clamp(((cc_alpha_in(aA, shade, in) - cc_alpha_in(bA, shade, in)) * cc_alpha_in(cA, shade, in)) +
                      cc_alpha_in(dA, shade, in));
}

/**
 * Run the combiner for one shade value. Both cycles, if othermode says so — the
 * directional-lighting materials are two-cycle and the second cycle is where the
 * environment blend lives, so stopping after cycle 0 (as we used to) throws away
 * the half that matters.
 */
static void combiner_run(const f32 shade[4], f32 out[4]) {
    f32 cycle0[4];
    s32 i;

    cc_cycle((sCombineW0 >> 20) & 0xF, (sCombineW1 >> 28) & 0xF, (sCombineW0 >> 15) & 0x1F, (sCombineW1 >> 15) & 0x7,
             (sCombineW0 >> 12) & 0x7, (sCombineW1 >> 12) & 0x7, (sCombineW0 >> 9) & 0x7, (sCombineW1 >> 9) & 0x7,
             shade, shade, cycle0);

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE) {
        cc_cycle((sCombineW0 >> 5) & 0xF, (sCombineW1 >> 24) & 0xF, sCombineW0 & 0x1F, (sCombineW1 >> 6) & 0x7,
                 (sCombineW1 >> 21) & 0x7, (sCombineW1 >> 3) & 0x7, (sCombineW1 >> 18) & 0x7, sCombineW1 & 0x7, shade,
                 cycle0, out);
    } else {
        for (i = 0; i < 4; i++) {
            out[i] = cycle0[i];
        }
    }
}

/**
 * Split the combiner into the two halves the fixed-function pipeline can apply:
 * the factor the texel is multiplied by, and the term added to it afterwards.
 *
 * Every combiner DKR uses is affine in TEXEL0 — result = TEXEL0 * K1 + K2 — so
 * running it twice, once with the texel white and once with it black, recovers
 * both halves: K2 is the result with no texel at all, and K1 is what the texel
 * added on top. GL then reproduces it exactly: K1 goes in the vertex colour and
 * modulates the texture, K2 in the secondary colour, which GL_COLOR_SUM adds
 * after texturing.
 *
 * This is what makes the menu's flashing highlight work. The font combiner is
 * G_CC_BLENDT_ENV_ALPHA_A_TxP — lerp(TEXEL0, ENV, ENV_ALPHA) — and font.c puts the
 * highlight colour in ENV and its pulse in ENV's alpha. Folding everything into a
 * single modulated colour, as we used to, computes texel * ENV: at full blend that
 * is texel * white, i.e. the original texel, and the flash silently cancels out.
 *
 * Alpha stays a plain modulate (GL_COLOR_SUM is RGB-only), which is exact for the
 * TEXEL0 * PRIM form DKR's 2D uses everywhere.
 */
static void combiner_eval(const f32 shade[4], u8 lit[4], u8 unlit[3]) {
    f32 on[4];  // texel = white
    f32 off[4]; // texel = black
    s32 i;

    sCcTexel = 1.0f;
    combiner_run(shade, on);
    sCcTexel = 0.0f;
    combiner_run(shade, off);
    sCcTexel = 1.0f;

    for (i = 0; i < 3; i++) {
        lit[i] = clamp_u8(on[i]);
        unlit[i] = clamp_u8(off[i]);
    }
    lit[3] = clamp_u8(on[3]);
}

/**
 * Just the lit half. The 3D path (project) only ever consumes `lit` — the
 * texel=0 run that combiner_eval also does is thrown away there, so the second
 * combiner_run was pure waste on every 3D vertex in the frame. The 2D path still
 * needs both halves and keeps using combiner_eval.
 */
static void combiner_eval_lit(const f32 shade[4], u8 lit[4]) {
    f32 on[4];
    s32 i;

    sCcTexel = 1.0f;
    combiner_run(shade, on);

    for (i = 0; i < 4; i++) {
        lit[i] = clamp_u8(on[i]);
    }
}

// ---------------------------------------------------------------------------
// Per-batch combiner classification.
//
// Running the mux per vertex measured at ~29ms a frame, the largest single cost
// in the renderer. But the mux, prim and env colours are constant for a whole
// batch; only shade varies. So for most batches each output channel is one of
// two things:
//
//   passthrough — the channel is the shade. A plain texel * shade modulate,
//                 which DKR's 3D geometry uses almost everywhere.
//   constant    — the channel never varies with shade; evaluate it once.
//
// When every channel is one of those the combiner collapses to four selects.
// Anything else falls back to the real mux, per vertex.
//
// Classification is per channel, not per batch: DKR's 3D materials are
// texel * shade on RGB but take alpha from PRIM or ENV for fades, so an
// all-or-nothing test would reject the batches worth catching.
//
// The test is functional, not a mux pattern-match: evaluate the real combiner on
// probe shades and inspect the output. An unrecognised combiner can't be
// misclassified; it fails both tests and takes the slow path.
// ---------------------------------------------------------------------------
// The state the classification is valid for. Any change and it is recomputed.
static u32 sCcSigW0, sCcSigW1, sCcSigCyc, sCcSigPrim, sCcSigEnv;
static s32 sCcSigValid = FALSE;
static u32 sCcCacheAt;

// Materials alternate within a frame, so remember more than the last result.
#define GFX_CC_CACHE 128
static struct {
    u32 w0, w1, cyc, prim, env;
    u8 valid, fast, pass[4], konst[4];
} sCcCache[GFX_CC_CACHE];

static void combiner_classify(void) {
    // Three probes, not two: two mid-range ones would call a combiner that
    // saturates at both of them constant (lerp-towards-env clamps to 255 at any
    // bright shade but not a dark one), so the third sits on the 0/255 extremes
    // where a saturating combiner shows itself.
    static const u8 probeA[4] = { 13, 71, 149, 233 };
    static const u8 probeB[4] = { 200, 5, 96, 44 };
    static const u8 probeC[4] = { 0, 255, 255, 0 };
    u32 cyc = sOtherModeH & (3 << G_MDSFT_CYCLETYPE);
    u32 prim = (sPrimColor[0] << 24) | (sPrimColor[1] << 16) | (sPrimColor[2] << 8) | sPrimColor[3];
    u32 env = (sEnvColor[0] << 24) | (sEnvColor[1] << 16) | (sEnvColor[2] << 8) | sEnvColor[3];
    f32 shade[4];
    u8 a[4], b[4], c[4];
    s32 i;

#if DKR_GFX_PROFILE
    sCcClassified++;
#endif
    if (sCcSigValid && sCombineW0 == sCcSigW0 && sCombineW1 == sCcSigW1 && cyc == sCcSigCyc && prim == sCcSigPrim &&
        env == sCcSigEnv) {
        return; // still the combiner we classified last time
    }
    sCcSigW0 = sCombineW0;
    sCcSigW1 = sCombineW1;
    sCcSigCyc = cyc;
    sCcSigPrim = prim;
    sCcSigEnv = env;
    sCcSigValid = TRUE;

    {
        u32 hash = sCombineW0 ^ (sCombineW1 * 0x9E3779B1u) ^ (prim * 0x85EBCA6Bu) ^ (env * 0xC2B2AE35u) ^ cyc;
        u32 at = (hash ^ (hash >> 15)) & (GFX_CC_CACHE - 1);

        if (sCcCache[at].valid && sCcCache[at].w0 == sCombineW0 && sCcCache[at].w1 == sCombineW1 &&
            sCcCache[at].cyc == cyc && sCcCache[at].prim == prim && sCcCache[at].env == env) {
            sCcFast = sCcCache[at].fast;
            for (i = 0; i < 4; i++) {
                sCcPass[i] = sCcCache[at].pass[i];
                sCcConst[i] = sCcCache[at].konst[i];
            }
            return;
        }
        sCcCacheAt = at;
    }
#if DKR_GFX_PROFILE
    sCcProbed++;
#endif

    for (i = 0; i < 4; i++) {
        shade[i] = probeA[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, a);
    for (i = 0; i < 4; i++) {
        shade[i] = probeB[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, b);
    for (i = 0; i < 4; i++) {
        shade[i] = probeC[i] * (1.0f / 255.0f);
    }
    combiner_eval_lit(shade, c);

    // Classify each channel on its own. Doing this all-or-nothing was the bug:
    // DKR's 3D materials are texel * shade on RGB but take alpha from PRIM or ENV
    // for fades, so a single non-passthrough channel used to drop the whole batch
    // onto the slow path — which is every batch that matters.
    sCcFast = TRUE;
    for (i = 0; i < 4; i++) {
        if (a[i] == probeA[i] && b[i] == probeB[i] && c[i] == probeC[i]) {
            sCcPass[i] = TRUE; // shade in, same shade out
            sCcConst[i] = 0;
        } else if (a[i] == b[i] && a[i] == c[i]) {
            sCcPass[i] = FALSE; // never varies with shade
            sCcConst[i] = a[i];
        } else {
            sCcFast = FALSE; // genuinely mixes shade with something else
            break;
        }
    }

    sCcCache[sCcCacheAt].w0 = sCombineW0;
    sCcCache[sCcCacheAt].w1 = sCombineW1;
    sCcCache[sCcCacheAt].cyc = cyc;
    sCcCache[sCcCacheAt].prim = prim;
    sCcCache[sCcCacheAt].env = env;
    sCcCache[sCcCacheAt].fast = (u8) sCcFast;
    for (i = 0; i < 4; i++) {
        sCcCache[sCcCacheAt].pass[i] = (u8) sCcPass[i];
        sCcCache[sCcCacheAt].konst[i] = sCcConst[i];
    }
    sCcCache[sCcCacheAt].valid = TRUE;
}

/**
 * The two colours a rectangle's vertices carry. A rectangle has no shade, so it
 * goes in as white and the combiner is constant across the quad — but it still has
 * to be split into its modulate and add halves, because the font blends the glyph
 * towards ENV and that add is the whole point (see combiner_eval).
 */
static void combiner_color(u8 lit[4], u8 unlit[3]) {
    static const f32 white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    combiner_eval(white, lit, unlit);
}

/**
 * Two triangles in screen space, which is all a rectangle is once the RDP is out
 * of the picture. Depth testing is off: the game clears G_ZBUFFER for its 2D and
 * relies on display-list order, and so do we.
 */
static void draw_2d_quad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, const u8 lit[4],
                         const u8 unlit[3], u32 texture) {
    GfxTriVert q[6];
    s32 i;

    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x1; q[2].y = y1; q[2].u = u1; q[2].v = v1;
    q[3].x = x0; q[3].y = y0; q[3].u = u0; q[3].v = v0;
    q[4].x = x1; q[4].y = y1; q[4].u = u1; q[4].v = v1;
    q[5].x = x0; q[5].y = y1; q[5].u = u0; q[5].v = v1;

    for (i = 0; i < 6; i++) {
        q[i].z = 0.0f;
        q[i].w = 1.0f; // already in screen space — nothing to undo
        q[i].fog = 0.0f;
        // Textured: the vertex carries the no-texel end of the combiner and the
        // texture env carries the full-texel end, and GL_BLEND lerps between them
        // by the texel — reproducing the combiner exactly. Untextured: there is no
        // texel to lerp with, so the vertex carries the whole result.
        q[i].r = (texture != 0) ? unlit[0] : lit[0];
        q[i].g = (texture != 0) ? unlit[1] : lit[1];
        q[i].b = (texture != 0) ? unlit[2] : lit[2];
        q[i].a = lit[3];
    }

    if (texture != 0) {
        gfx_set_texenv_blend(lit);
    } else {
        gfx_set_texenv_modulate();
    }

    // The 2D layer is ordered by the display list, not by depth. Everything the
    // last material batch left set has to be undone; the next G_TRIN re-applies
    // its own state through apply_render_mode(), so nothing needs restoring.
    gfx_set_depth_test(FALSE);
    gfx_set_depth_write(FALSE);
    gfx_set_depth_offset(FALSE);
    gfx_set_alpha_test(0.0f);
    gfx_set_fog(FALSE, sFogColor); // the HUD does not sit in the world's haze
    gfx_bind_texture(texture);
    apply_texture_filter();
    gfx_draw_tris(q, 6);
}

/**
 * G_TEXRECT / G_TEXRECTFLIP — the whole 2D layer: every glyph, HUD sprite and
 * menu image. `w2`/`w3` are the two G_RDPHALF words that follow the opcode and
 * carry the texture coordinates.
 */
static void handle_texrect(u32 w0, u32 w1, u32 w2, u32 w3, s32 flip) {
    PROF_ADD(rects, 1);
    // Screen coordinates are 10.2 fixed point, texture coordinates S10.5, and
    // the per-pixel texture steps S5.10.
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    f32 s0 = (f32) (s16) (w2 >> 16) / 32.0f;
    f32 t0 = (f32) (s16) (w2 & 0xFFFF) / 32.0f;
    f32 dsdx = (f32) (s16) (w3 >> 16) / 1024.0f;
    f32 dtdy = (f32) (s16) (w3 & 0xFFFF) / 1024.0f;
    f32 s1, t1;
    u8 lit[4];
    u8 unlit[3];
    u32 texture = texture_current();

    if (texture == 0) {
        return;
    }

    // A flipped rect walks s down the rectangle's height and t across its width.
    if (flip) {
        s1 = s0 + ((y1 - y0) * dsdx);
        t1 = t0 + ((x1 - x0) * dtdy);
    } else {
        s1 = s0 + ((x1 - x0) * dsdx);
        t1 = t0 + ((y1 - y0) * dtdy);
    }

    // The texel coordinates are relative to the image, but the texture we
    // uploaded starts at the tile's upper-left corner.
    s0 -= sTileUls / 4.0f;
    s1 -= sTileUls / 4.0f;
    t0 -= sTileUlt / 4.0f;
    t1 -= sTileUlt / 4.0f;

    combiner_color(lit, unlit);
    draw_2d_quad(x0, y0, x1, y1, s0 / sTileWidth, t0 / sTileHeight, s1 / sTileWidth, t1 / sTileHeight, lit, unlit,
                 texture);
}

/**
 * G_FILLRECT — the letterbox bars, dialogue-box backgrounds and screen fades.
 */
static void handle_fillrect(u32 w0, u32 w1) {
    f32 x0 = ((w1 >> 12) & 0xFFF) / 4.0f;
    f32 y0 = (w1 & 0xFFF) / 4.0f;
    f32 x1 = ((w0 >> 12) & 0xFFF) / 4.0f;
    f32 y1 = (w0 & 0xFFF) / 4.0f;
    u8 color[4];
    u8 unlit[3] = { 0, 0, 0 };

    if ((sOtherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_FILL) {
        // Fill mode paints the fill colour flat, and its lower-right corner is
        // inclusive — one pixel further than the same rect in 1-cycle mode. The
        // colour's alpha bit is the framebuffer's coverage bit, not
        // transparency: the rect is opaque whichever way it is set.
        color[0] = sFillColor[0];
        color[1] = sFillColor[1];
        color[2] = sFillColor[2];
        color[3] = 0xFF;
        x1 += 1.0f;
        y1 += 1.0f;
    } else {
        combiner_color(color, unlit);
        if (color[3] == 0) {
            return; // fully transparent — the blender would discard it anyway
        }
    }

    draw_2d_quad(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, color, unlit, 0);
}

/**
 * Interpret one display list. A `count` above zero means "at most this many
 * commands" — the task's own display list is bounded by its length rather than
 * a G_ENDDL, and G_DMADL likewise DMAs a fixed-size block. Zero means "run
 * until G_ENDDL", which is how nested G_DL lists terminate.
 */
static void run_dl(const Gfx *dl, s32 count, s32 depth) {
    s32 i;

    if (dl == NULL || depth > GFX_MAX_DEPTH) {
        return;
    }

    for (i = 0; count == 0 || i < count; i++) {
        u32 w0 = dl[i].words.w0;
        u32 w1 = dl[i].words.w1;
        u32 cmd = (w0 >> 24) & 0xFF;

        // The immediate-mode opcodes are negative ints in gbi.h (G_ENDDL is
        // G_IMMFIRST-7 = -72), so every case has to be masked to the byte the
        // display list actually holds.
        switch (cmd) {
            case G_MTX: {
                s32 slot = (((w0 >> 16) & 0xFF) >> 6) & 3;
                if (w1 != 0 && slot < 3) {
                    PROF_TIME(tMtx, mtx_to_float((const Mtx *) w1, sMatrices[slot]));
                    sCurMatrix = slot;
                }
                break;
            }
            case (u8) G_MOVEWORD: {
                u32 index = w0 & 0xFF;
                if (index == G_MW_MVPMATRIX) {
                    sCurMatrix = (w1 >> 6) & 3;
                } else if (index == G_MW_BILLBOARD) {
                    sBillboard = (w1 != 0);
                } else if (index == G_MW_FOG) {
                    // gSPFogPosition packs the multiplier in the high half and the
                    // offset in the low half; both are signed.
                    sFogMul = (s16) (w1 >> 16);
                    sFogOfs = (s16) (w1 & 0xFFFF);
                }
                break;
            }
            case G_MOVEMEM: {
                // gSPViewport goes through gDma1p, which packs the parameter at
                // bits 16-23 and the *length* at 0-15 (include/PR/gbi.h). Reading
                // the low byte matched the length, never the parameter, so the
                // viewport was silently ignored: everything rendered at the
                // 320x240 default, which looks right full-screen and falls apart
                // the moment the game asks for a smaller one.
                if (((w0 >> 16) & 0xFF) == G_MV_VIEWPORT && w1 != 0) {
                    const Vp *vp = (const Vp *) w1;
                    f32 halfW, halfH;

                    sVpScaleX = vp->vp.vscale[0] / 4.0f;
                    sVpScaleY = vp->vp.vscale[1] / 4.0f;
                    sVpTransX = vp->vp.vtrans[0] / 4.0f;
                    sVpTransY = vp->vp.vtrans[1] / 4.0f;

                    // The rectangle NDC +-1 maps onto. The scale carries a sign
                    // (y is commonly negative, since screen y runs the other way
                    // from clip y), so take it off before using it as a half-size.
                    halfW = (sVpScaleX < 0.0f) ? -sVpScaleX : sVpScaleX;
                    halfH = (sVpScaleY < 0.0f) ? -sVpScaleY : sVpScaleY;
                    sVpClipX0 = sVpTransX - halfW;
                    sVpClipX1 = sVpTransX + halfW;
                    sVpClipY0 = sVpTransY - halfH;
                    sVpClipY1 = sVpTransY + halfH;
                    apply_clip_rect();
                }
                break;
            }
            case G_VTX: {
#if DKR_GFX_PROFILE
                if ((sCatCur == PC_PROF_CAT_LEVEL || sCatCur == PC_PROF_CAT_LEVEL_TR) && sLevelCullMatrix != NULL) {
                    sScene.levelVtx++;
                    if (memcmp(sMatrices[sCurMatrix], sLevelCullMatrix, sizeof(sMatrices[0])) != 0) {
                        sScene.levelVtxOtherMatrix++;
                    }
                }
#endif
                // Peephole: a G_VTX immediately followed by the one G_TRIN that
                // consumes it is the common object/model batch shape, and the
                // pair can be fused into the native blast path — skipping the
                // sVerts staging and the whole per-corner emit machinery.
                //
                // Object models whose batch hangs off a second matrix arrive in
                // two more shapes (see the batch loop in src/objects.c), both
                // with matrix selects around the load:
                //   G_VTX, select, G_TRIN                      one matrix
                //   G_VTX, select, G_VTX append, select, G_TRIN  two matrices
                // A select only changes which matrix later loads use, so the
                // vertices transform with the matrix current at their own G_VTX
                // and the last select is applied afterwards.
                //
                // Guards: no append on the first load (the anchor scheme needs
                // sVerts), no billboard, and — because the fast path leaves
                // sVerts stale — the command after the batch must not be another
                // G_TRIN reusing these vertices. When blast_render itself falls
                // back it does so by running the real handlers, which
                // repopulates sVerts, so every fallback is exactly the unfused
                // behaviour.
                u32 params = (w0 >> 16) & 0xFF;

                if (!(params & G_VTX_APPEND) && !sBillboard) {
#define DL_HAS(k) (count == 0 || i + (k) < count)
#define DL_OP(k) ((dl[i + (k)].words.w0 >> 24) & 0xFF)
#define DL_SELECT(k) (DL_OP(k) == (u8) G_MOVEWORD && (dl[i + (k)].words.w0 & 0xFF) == G_MW_MVPMATRIX)
#define DL_LAST_TRIN(k) (DL_OP(k) == G_TRIN && !(DL_HAS((k) + 1) && DL_OP((k) + 1) == G_TRIN))
                    s32 numVerts = (s32) ((params >> 3) & 0x1F) + 1;
                    const Vertex *verts2 = NULL;
                    s32 numVerts2 = 0, matrix2 = 0, trin = 0, select = 0;

                    if (DL_HAS(1) && DL_LAST_TRIN(1)) {
                        trin = 1;
                    } else if (DL_HAS(2) && DL_SELECT(1) && DL_LAST_TRIN(2)) {
                        trin = 2;
                        select = 1;
                    } else if (DL_HAS(4) && DL_SELECT(1) && DL_OP(2) == G_VTX &&
                               ((dl[i + 2].words.w0 >> 16) & G_VTX_APPEND) && DL_SELECT(3) && DL_LAST_TRIN(4)) {
                        u32 params2 = (dl[i + 2].words.w0 >> 16) & 0xFF;

                        numVerts2 = (s32) ((params2 >> 3) & 0x1F) + 1;
                        if (numVerts + numVerts2 <= GFX_BLAST_MAX_VERTS && dl[i + 2].words.w1 != 0) {
                            verts2 = (const Vertex *) dl[i + 2].words.w1;
                            matrix2 = (dl[i + 1].words.w1 >> 6) & 3;
                            trin = 4;
                            select = 3;
                        }
                    }
#undef DL_HAS
#undef DL_OP
#undef DL_SELECT
#undef DL_LAST_TRIN
                    if (trin) {
                        u32 tw0 = dl[i + trin].words.w0;

                        PROF_TIME(tBlast, blast_render((const Vertex *) w1, numVerts, verts2, numVerts2, matrix2,
                                                       (const Triangle *) dl[i + trin].words.w1,
                                                       (s32) (((tw0 >> 16) & 0xFF) >> 4) + 1,
                                                       (s32) ((tw0 >> 16) & 1)));
                        if (select) {
                            sCurMatrix = (dl[i + select].words.w1 >> 6) & 3;
                        }
                        i += trin;
                        break;
                    }
                }
#if DKR_GFX_PROFILE
                sProfVtxWhy = (params & G_VTX_APPEND) ? 1 : sBillboard ? 2 :
                              (((dl[i + 1].words.w0 >> 24) & 0xFF) == G_TRIN) ? 3 : 4;
#endif
                PROF_TIME(tVtx, handle_vertex(w0, w1));
                break;
            }
            case G_TRIN:
#if DKR_GFX_PROFILE
                {
                    u32 n = (((w0 >> 16) & 0xFF) >> 4) + 1;
                    if (sProfVtxWhy == 1) sProf.whyAppend += n;
                    else if (sProfVtxWhy == 2) sProf.whyBillboard += n;
                    else if (sProfVtxWhy == 3) sProf.whyShared += n;
                    else sProf.whyLoose += n;
                }
#endif
                PROF_TIME(tPoly, handle_polygon(w0, w1));
                break;
            case G_DL:
                run_dl((const Gfx *) w1, 0, depth + 1);
                if (((w0 >> 16) & 0xFF) == G_DL_NOPUSH) {
                    return; // a branch, not a call
                }
                break;
            case (u8) G_NOOP:
                if (w1 == PC_SCRAWL_TAG) {
                    pc_scrawl_draw();
                } else if (w1 == PC_SWIRL_TAG) {
                    pc_swirl_draw();
                } else if (w1 == PC_COVER_BEGIN_TAG || w1 == PC_COVER_END_TAG) {
                    gfx_set_cover(w1 == PC_COVER_BEGIN_TAG);
                }
#if DKR_GFX_PROFILE
                if ((w1 & 0xFFFFFF00u) == PC_PROF_CAT_TAG) {
                    scene_category((s32) (w1 & 0xFF));
                }
#endif
                break;
            case G_DMADL:
                run_dl((const Gfx *) w1, (w0 >> 16) & 0xFF, depth + 1);
                break;
            case (u8) G_SETTIMG:
                sTimgAddr = w1;
                break;
            case (u8) G_SETTILE: {
                // Only the tile the triangles actually render with matters.
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    sTileFmt = (w0 >> 21) & 0x7;
                    sTileSiz = (w0 >> 19) & 0x3;
                    sTilePalette = (w1 >> 20) & 0xF;
                    // cmt/cms are 2-bit fields (bit 0 mirror, bit 1 clamp), not
                    // flags. Reading only the low bit picked up mirror and
                    // dropped clamp entirely, so every clamped texture wrapped.
                    sTileCmT = (w1 >> 18) & 0x3;
                    sTileCmS = (w1 >> 8) & 0x3;
                    sTexStateDirty = TRUE;
                }
                break;
            }
            case (u8) G_SETTILESIZE: {
                s32 tile = (w1 >> 24) & 0x7;
                if (tile == G_TX_RENDERTILE) {
                    // uls/ult/lrs/lrt are 10.2 fixed point, inclusive bounds.
                    u32 uls = (w0 >> 12) & 0xFFF;
                    u32 ult = w0 & 0xFFF;
                    u32 lrs = (w1 >> 12) & 0xFFF;
                    u32 lrt = w1 & 0xFFF;
                    sTileWidth = ((lrs - uls) >> 2) + 1;
                    sTileHeight = ((lrt - ult) >> 2) + 1;
                    sTexStateDirty = TRUE;
                    sTileUls = uls;
                    sTileUlt = ult;
                }
                break;
            }
            case (u8) G_LOADBLOCK:
                // Latch the image being loaded: gDPLoadTLUT_pal16 issues its own
                // G_SETTIMG for the palette afterwards, which would otherwise
                // overwrite the texture address before the triangles draw.
                sTexAddr = sTimgAddr;
                // dxt is the low 12 bits. Zero means the RDP applied no odd-row
                // swizzle on the way in, i.e. the asset is already swizzled.
                sTexSwapped = ((w1 & 0xFFF) == 0);
                sTexStateDirty = TRUE;
                break;
            case (u8) G_LOADTILE:
                sTexAddr = sTimgAddr;
                sTexSwapped = FALSE; // a tile load is row-by-row, never swizzled
                sTexStateDirty = TRUE;
                break;
            case (u8) G_LOADTLUT:
                // The palette is whatever G_SETTIMG last pointed at.
                sTlutAddr = sTimgAddr;
                sTexStateDirty = TRUE;
                break;
            case (u8) G_TEXRECT:
            case (u8) G_TEXRECTFLIP: {
                // A texture rectangle is three display-list words: the opcode,
                // then G_RDPHALF_1 and G_RDPHALF_2 carrying the texture
                // coordinates. Skip past them so they are not run as commands.
                if (count != 0 && i + 2 >= count) {
                    return; // truncated — the halves are not there to read
                }
                PROF_TIME(tRect, handle_texrect(w0, w1, dl[i + 1].words.w1, dl[i + 2].words.w1,
                                                cmd == (u8) G_TEXRECTFLIP));
                i += 2;
                break;
            }
            case (u8) G_FILLRECT:
                PROF_TIME(tRect, handle_fillrect(w0, w1));
                break;
            case (u8) G_SETSCISSOR: {
                // 10.2 fixed point, lower-right inclusive — a full-screen scissor
                // arrives as (0, 0, 319, 239), hence the +1 to make it exclusive.
                // This is what clips the dialogue box's scrolling text reveal
                // (src/font.c) and, with the viewport, each player's half.
                sScisX0 = ((w0 >> 12) & 0xFFF) / 4.0f;
                sScisY0 = (w0 & 0xFFF) / 4.0f;
                sScisX1 = (((w1 >> 12) & 0xFFF) / 4.0f) + 1.0f;
                sScisY1 = ((w1 & 0xFFF) / 4.0f) + 1.0f;
                apply_clip_rect();
                break;
            }
            case (u8) G_SETGEOMETRYMODE:
                sGeometryMode |= w1;
                break;
            case (u8) G_CLEARGEOMETRYMODE:
                sGeometryMode &= ~w1;
                break;
            case (u8) G_SETCOMBINE:
                sCombineW0 = w0;
                sCombineW1 = w1;
                break;
            case (u8) G_SETPRIMCOLOR:
                sPrimColor[0] = (w1 >> 24) & 0xFF;
                sPrimColor[1] = (w1 >> 16) & 0xFF;
                sPrimColor[2] = (w1 >> 8) & 0xFF;
                sPrimColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETFOGCOLOR:
                sFogColor[0] = (w1 >> 24) & 0xFF;
                sFogColor[1] = (w1 >> 16) & 0xFF;
                sFogColor[2] = (w1 >> 8) & 0xFF;
                sFogColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETENVCOLOR:
                sEnvColor[0] = (w1 >> 24) & 0xFF;
                sEnvColor[1] = (w1 >> 16) & 0xFF;
                sEnvColor[2] = (w1 >> 8) & 0xFF;
                sEnvColor[3] = w1 & 0xFF;
                break;
            case (u8) G_SETFILLCOLOR: {
                // Two packed RGBA5551 texels, one per 16-bit half of the word;
                // the game always sets both to the same colour.
                u16 texel = w1 & 0xFFFF;
                u32 rgba = rgba16_to_rgba32(texel);
                sFillColor[0] = rgba & 0xFF;
                sFillColor[1] = (rgba >> 8) & 0xFF;
                sFillColor[2] = (rgba >> 16) & 0xFF;
                sFillColor[3] = (rgba >> 24) & 0xFF;
                break;
            }
            case (u8) G_SETOTHERMODE_H:
            case (u8) G_SETOTHERMODE_L: {
                // F3D encodes the field as a shift and a bit count, with the
                // data already sitting at its final position.
                u32 shift = (w0 >> 8) & 0xFF;
                u32 length = w0 & 0xFF;
                u32 mask = (length >= 32) ? 0xFFFFFFFF : (((u32) 1 << length) - 1) << shift;

                if (cmd == (u8) G_SETOTHERMODE_H) {
                    sOtherModeH = (sOtherModeH & ~mask) | (w1 & mask);
                } else {
                    sOtherModeL = (sOtherModeL & ~mask) | (w1 & mask);
                }
                break;
            }
            case (u8) G_RDPSETOTHERMODE:
                // Both halves at once: the high word is packed into w0, the low
                // word — render mode, alpha compare, z mode — is all of w1. This
                // is the form every DKR material arrives in (gsDPSetOtherMode).
                sOtherModeH = w0 & 0x00FFFFFF;
                sOtherModeL = w1;
                break;
            case (u8) G_SETBLENDCOLOR:
                // The alpha is the reference value a G_AC_THRESHOLD compare uses.
                sBlendColor[0] = (w1 >> 24) & 0xFF;
                sBlendColor[1] = (w1 >> 16) & 0xFF;
                sBlendColor[2] = (w1 >> 8) & 0xFF;
                sBlendColor[3] = w1 & 0xFF;
                break;
            case (u8) G_ENDDL:
                return;
            default:
                // Everything else is RDP state we do not model (blenders,
                // scissors, texture filters) plus the G_RDPHALF words consumed
                // by G_TEXRECT above.
                break;
        }
    }
}

extern void dc_audio_init(void);
extern void dc_audio_start_thread(void);

// Sampling profile (GFX_PROFILE=3): a timer interrupt notes where the program
// is, a few thousand times a second, while a race level (or PROF_SAMPLE_MAP)
// is loaded. Unlike the timers above it costs next to nothing, so it shows
// where time really goes.
// The counts land in build/dc/prof_samples.bin when the run exits by itself;
// one count per 32 bytes of code from the load address.
#if DKR_GFX_PROFILE >= 3
#define PROF_SAMPLE_BASE 0x8c010000u
#define PROF_SAMPLE_BUCKETS (1536u * 1024u / 32u) // the program's code is about 1.1 MB
extern s32 gMapId;
static u16 sProfSamples[PROF_SAMPLE_BUCKETS];
static u32 sProfSampleInfo[4]; // samples taken, samples outside the range, bucket bytes, base

static void prof_sample_count(u32 pc) {
    u32 bucket = (pc - PROF_SAMPLE_BASE) / 32u;

    sProfSampleInfo[0]++;
    if (bucket >= PROF_SAMPLE_BUCKETS) {
        sProfSampleInfo[1]++;
    } else if (sProfSamples[bucket] != 0xFFFF) {
        sProfSamples[bucket]++;
    }
}

#if DKR_GFX_PROFILE >= 4
// GFX_PROFILE=4: only the late frames. A frame's samples wait here and are
// counted once the next frame start shows that it was late.
#define PROF_FRAME_SAMPLES 512
static u32 sProfFramePc[PROF_FRAME_SAMPLES];
static u32 sProfFrameCount;

static void prof_sample_frame_end(s32 late) {
    int old = irq_disable();
    u32 i;

    if (late) {
        for (i = 0; i < sProfFrameCount; i++) {
            prof_sample_count(sProfFramePc[i]);
        }
    }
    sProfFrameCount = 0;
    irq_restore(old);
}
#else
#define prof_sample_frame_end(late) ((void) (late))
#endif

static void prof_sample_irq(irq_t code, irq_context_t *context, void *data) {
    (void) code;
    (void) data;
    timer_clear(TMU1);
#if DKR_PROF_SAMPLE_MAP >= 0
    if (gMapId != DKR_PROF_SAMPLE_MAP) {
        return;
    }
#else
    if (gMapId != 18 && gMapId != 28) {
        return;
    }
#endif
#if DKR_GFX_PROFILE >= 4
    if (sProfFrameCount < PROF_FRAME_SAMPLES) {
        sProfFramePc[sProfFrameCount++] = context->pc;
    }
#else
    prof_sample_count(context->pc);
#endif
}

static void prof_sample_start(void) {
    sProfSampleInfo[2] = 32;
    sProfSampleInfo[3] = PROF_SAMPLE_BASE;
    irq_set_handler(EXC_TMU1_TUNI1, prof_sample_irq, NULL);
    timer_prime(TMU1, 3989, 1); // not a multiple of the frame rate
    timer_start(TMU1);
}

static void prof_sample_write(void) {
    file_t file;

    timer_stop(TMU1);
    file = fs_open("/pc/build/dc/prof_samples.bin", O_WRONLY | O_CREAT | O_TRUNC);
    if (file != FILEHND_INVALID) {
        fs_write(file, sProfSampleInfo, sizeof(sProfSampleInfo));
        fs_write(file, sProfSamples, sizeof(sProfSamples));
        fs_close(file);
    }
}
#else
#define prof_sample_start() ((void) 0)
#define prof_sample_write() ((void) 0)
#define prof_sample_frame_end(late) ((void) (late))
#endif

// Debug view (DEBUG_VIEW=1): hold a frame and switch how cutout textures are
// stored and drawn, to compare the modes on a still picture.
//
//   both triggers + X   hold or release the current frame
//   both triggers + Y   next mode; the squares at the top left show which
//
//   1  as before: 4-4-4-4
//   2  5-5-5-1
//   3  5-5-5-1, invisible texels coloured from their neighbours (the default)
#ifndef DKR_DEBUG_VIEW
#define DKR_DEBUG_VIEW 0
#endif
#if DKR_DEBUG_VIEW
extern void gfx_set_cutout_mode(int use1555, int bleed);
static s32 sDebugHold;
static s32 sDebugMode = 3;

static void debug_view_apply(void) {
    static const u8 modes[3][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 } };
    s32 i;

    gfx_set_cutout_mode(modes[sDebugMode - 1][0], modes[sDebugMode - 1][1]);
    // The format is chosen when a texture is created, so make them all anew.
    for (i = 0; i < sTexCacheCount; i++) {
        if (!sTexCache[i].prebuilt) {
            gfx_delete_texture(sTexCache[i].handle);
        }
    }
    sTexCacheCount = 0;
    memset(sTexHint, 0, sizeof(sTexHint));
    sTexStateDirty = TRUE;
    printf("DEBUG VIEW: mode %d\n", (int) sDebugMode);
}

static void debug_view_poll(void) {
    static u32 sLast;
    maple_device_t *pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *state = pad ? (cont_state_t *) maple_dev_status(pad) : NULL;
    u32 buttons = (state && state->ltrig > 128 && state->rtrig > 128) ? state->buttons : 0;
    u32 pressed = buttons & ~sLast;

    sLast = buttons;
    if (pressed & CONT_X) {
        sDebugHold = !sDebugHold;
        printf("DEBUG VIEW: frame %s\n", sDebugHold ? "held" : "released");
    }
    if (pressed & CONT_Y) {
        sDebugMode = (sDebugMode % 3) + 1;
        debug_view_apply();
    }
}

static void debug_view_draw(void) {
    static const u8 white[4] = { 255, 255, 255, 255 }, black[3] = { 0, 0, 0 };
    s32 i;

    for (i = 0; i < sDebugMode; i++) {
        draw_2d_quad(8.0f + (i * 8.0f), 8.0f, 14.0f + (i * 8.0f), 14.0f, 0.0f, 0.0f, 0.0f, 0.0f, white, black, 0);
    }
}
#endif

static void gfx_task_draw(void *dlBegin, void *dlEnd);

#if DKR_ENJ_LOOP
static void gfx_task_queue(void *dlBegin, void *dlEnd);
#endif

void pc_gfx_task_submit(void *dlBegin, void *dlEnd) {
#if DKR_ENJ_LOOP
    // enDjinn draws it once the frame's iteration returns (pc_enj_loop_run).
    gfx_task_queue(dlBegin, dlEnd);
    return;
#endif
    gfx_task_draw(dlBegin, dlEnd);
    pc_scene_frame_shown();
#if DKR_DEBUG_VIEW
    // The game does not run while a frame is held: its last display list is
    // drawn again each field, so a mode change shows on the same picture.
    debug_view_poll();
    while (sDebugHold) {
        gfx_task_draw(dlBegin, dlEnd);
        debug_view_poll();
    }
#endif
}

typedef struct {
    void *begin, *end;
} FrameDl;
#if DKR_GFX_PROFILE
static u64 sProfT1, sProfT2; // the walk's start and end, for prof_frame
static s32 sProfStallKind;   // which stall the counter counts this frame: 0 data, 1 code
#endif

/** One frame's display-list walk, inside the opaque list. */
static void frame_walk(void *data) {
    const FrameDl *frame = data;

#if DKR_GFX_PROFILE
    sProfT1 = pc_uptime_ns();
    {
        // The previous scene was flipped by now (gfx_frame_begin waits for the
        // PVR), so the vblank count between two frame starts is how many
        // fields the frame before took.
        u32 vbl = (u32) pvr_get_vbl_count();
        if (sSceneVbl != 0 && pc_scene_is_ready()) {
            u32 fields = vbl - sSceneVbl;
            prof_sample_frame_end(fields > (u32) PC_LOGIC_UPDATE_RATE);
            sScene.frames++;
            if (fields > (u32) PC_LOGIC_UPDATE_RATE) {
                sScene.lateFrames++;
                sScene.lateFields += fields - (u32) PC_LOGIC_UPDATE_RATE;
            }
            if (fields > sScene.maxFields) sScene.maxFields = fields;
        } else {
            prof_sample_frame_end(0);
        }
        sSceneVbl = vbl;
    }
    sCatCur = PC_PROF_CAT_OTHER;
    sCatStart = sProfT1;
    sCatTrisStart = sProf.tris;
    {
        // The second performance counter (the first is KOS's clock) counts the
        // cycles the pipeline waits on cache misses, during the walk only:
        // operand-cache and instruction-cache misses take turns, a frame each.
        // Starting it clears it.
        sProfWalkSp = (u32) __builtin_frame_address(0);
        sProfStallKind ^= 1;
        perf_cntr_start(PRFC1, sProfStallKind ? PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE :
                                                PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, PMCR_COUNT_CPU_CYCLES);
    }
#endif

    sCurMatrix = 0;
    sBillboard = FALSE;
    sVertexBase = 0;
    sTriVertCount = 0;
    sTimgAddr = 0;
    sTexAddr = 0;
    sTexSwapped = 0;
    sTlutAddr = 0;
    sTileWidth = 0;
    sTileHeight = 0;
    sTexStateDirty = TRUE;
    sTileUls = 0;
    sTileUlt = 0;
    sOtherModeL = 0;
    sGeometryMode = GFX_GEOMETRY_MODE_INIT;

    // The list sets its own scissor and viewport before it draws anything
    // (src/rcp_dkr.c), but don't inherit last frame's rects until it does.
    sScisX0 = sVpClipX0 = 0.0f;
    sScisY0 = sVpClipY0 = 0.0f;
    sScisX1 = sVpClipX1 = (f32) N64_SCREEN_W;
    sScisY1 = sVpClipY1 = (f32) N64_SCREEN_H;
    gfx_disable_scissor();

    run_dl((const Gfx *) frame->begin, (const Gfx *) frame->end - (const Gfx *) frame->begin, 0);
#if DKR_GFX_PROFILE
    scene_category(PC_PROF_CAT_OTHER);
#endif
#if DKR_DEBUG_VIEW
    debug_view_draw();
#endif
#if DKR_GFX_PROFILE
    sProfT2 = pc_uptime_ns();
    sScene.walkStall[sProfStallKind] += perf_cntr_count(PRFC1);
    {
        int pt, tr;
        gfx_recorded_words(&pt, &tr);
        {
            extern void gfx_tr_stats(unsigned int out[10]);
            unsigned int tr8[10];
            s32 j;

            gfx_tr_stats(tr8);
            {
                extern void gfx_nodepth_stats(unsigned int out[4]);
                unsigned int nd[4];

                gfx_nodepth_stats(nd);
                for (j = 0; j < 4; j++) {
                    sProf.noDepth[j] += nd[j];
                }
            }
            for (j = 0; j < 10; j++) {
                sProf.trStats[j] += tr8[j];
            }
        }
        if ((u32) pt > sProf.ptWords) sProf.ptWords = (u32) pt;
        if ((u32) tr > sProf.trWords) sProf.trWords = (u32) tr;
    }
#endif

}

static void gfx_task_draw(void *dlBegin, void *dlEnd) {
    FrameDl frame = { dlBegin, dlEnd };
#if DKR_GFX_PROFILE
    u64 profT0 = pc_uptime_ns();
#endif
    sGfxFrameCount++;

#if DKR_ENJ_RENDER
    gfx_frame_render(frame_walk, &frame);
#else
    gfx_frame_begin();
    frame_walk(&frame);
    gfx_frame_end();
#endif
#if DKR_GFX_PROFILE
    prof_frame(profT0, sProfT1, sProfT2, pc_uptime_ns());
#endif
#if DKR_AUTO_EXIT_SECONDS
    // Unattended measurement runs: return to the loader by ourselves.
    if (pc_uptime_ns() > (u64) DKR_AUTO_EXIT_SECONDS * 1000000000ull) {
#if DKR_GFX_PROFILE
        pc_scene_prof_report();
#endif
        prof_sample_write();
#if DKR_GFX_PROFILE
        pc_prof_log_flush();
#endif
#if DKR_ASSET_TRACE
        {
            extern void pc_asset_trace_report(void);
            pc_asset_trace_report();
        }
#endif
        arch_exit();
    }
#endif
}

#if DKR_ENJ_LOOP
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>
// ENJ_LOOP=1: enDjinn's enj_state_run is the game loop. Each frame it calls
// the game's iteration (thread3_frame), which queues the display list it built
// through gfx_task_queue; enDjinn then submits the scene, and its post-render
// callback finishes the frame as gfx_task_draw's tail does.
static FrameDl sQueuedFrame;
#if DKR_GFX_PROFILE
static u64 sQueuedT0;
#endif

static void gfx_task_queue(void *dlBegin, void *dlEnd) {
    sQueuedFrame.begin = dlBegin;
    sQueuedFrame.end = dlEnd;
#if DKR_GFX_PROFILE
    sQueuedT0 = pc_uptime_ns();
#endif
    sGfxFrameCount++;
    gfx_frame_queue(frame_walk, &sQueuedFrame);
}

static void enj_frame_submitted(void *data) {
    (void) data;
    if (sQueuedFrame.begin == NULL) {
        return; // nothing was drawn this frame
    }
    sQueuedFrame.begin = NULL;
    gfx_frame_submitted();
    pc_scene_frame_shown();
#if DKR_GFX_PROFILE
    prof_frame(sQueuedT0, sProfT1, sProfT2, pc_uptime_ns());
#endif
#if DKR_AUTO_EXIT_SECONDS
    if (pc_uptime_ns() > (u64) DKR_AUTO_EXIT_SECONDS * 1000000000ull) {
#if DKR_GFX_PROFILE
        pc_scene_prof_report();
#endif
        prof_sample_write();
#if DKR_GFX_PROFILE
        pc_prof_log_flush();
#endif
#if DKR_ASSET_TRACE
        {
            extern void pc_asset_trace_report(void);
            pc_asset_trace_report();
        }
#endif
        arch_exit();
    }
#endif
}

void pc_enj_loop_run(void (*frame)(void *)) {
    static enj_mode_t sGameMode;

    sGameMode.mode_updater = frame;
    sGameMode.no_soft_reset = 1;
    strncpy(sGameMode.name, "dkr", sizeof(sGameMode.name) - 1);
    enj_render_post_callback_set(enj_frame_submitted, NULL);
    enj_mode_push(&sGameMode);
    enj_state_run();
}
#endif

static void cont_reset_btn_callback_(uint8_t addr, uint32_t btns) {
    (void)addr;
    (void)btns;
    arch_exit();
}

int main(int argc, char **argv) {
    cont_btn_callback(0, CONT_RESET_BUTTONS, cont_reset_btn_callback_);
    prof_sample_start();

    printf("=== DKR PC ===\n");
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
    gfx_window_init(N64_SCREEN_W, N64_SCREEN_H, WINDOW_SCALE);
    // Bring AICA up before init_game produces the first PCM, then hand audio to
    // its own vblank-driven thread so it survives the blocking level loads.
    dc_audio_init();
    dc_audio_start_thread();
    thread3_main(0);
    return 0;
}
