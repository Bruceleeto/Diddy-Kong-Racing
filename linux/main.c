#include <stdio.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>

#include "gfx.h"

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread, same as
// the OoT DC port calling Main()/Graph_ThreadEntry directly — the N64 boot
// chain (mainproc -> thread1 -> thread3) is skipped entirely, so the libultra
// thread machinery is never needed.
void thread3_main(void *unused);

// libultra code asks about "the current thread" (osGetThreadPri(NULL) etc.).
// There is no thread system on PC, so the host thread poses as one: thread 3
// at its real priority.
extern OSThread *__osRunningThread;
static OSThread sHostThread;

// ---------------------------------------------------------------------------
// Frame pacing — stands in for the VI retrace interrupt. Sleeps to the next
// 60Hz boundary and returns how many retrace periods passed since last call
// (the game uses that count as its update rate / frame-skip signal).
// ---------------------------------------------------------------------------
#include <time.h>

#define PC_RETRACE_NSEC (1000000000ll / 60)

s32 pc_retrace_wait(void) {
    static long long sLastNs = 0;
    struct timespec ts;
    long long nowNs;
    s32 periods;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    nowNs = (long long) ts.tv_sec * 1000000000ll + ts.tv_nsec;

    if (sLastNs == 0) {
        sLastNs = nowNs;
        return 1;
    }

    // Sleep until the next retrace boundary if we're early.
    if (nowNs - sLastNs < PC_RETRACE_NSEC) {
        long long targetNs = sLastNs + PC_RETRACE_NSEC;
        struct timespec req;
        req.tv_sec = (targetNs - nowNs) / 1000000000ll;
        req.tv_nsec = (targetNs - nowNs) % 1000000000ll;
        nanosleep(&req, NULL);
        nowNs = targetNs;
    }

    periods = (s32) ((nowNs - sLastNs) / PC_RETRACE_NSEC);
    if (periods < 1) {
        periods = 1;
    }
    if (periods > 6) { // don't let a debugger pause turn into a huge skip
        periods = 6;
    }
    sLastNs = nowNs;
    return periods;
}

// ---------------------------------------------------------------------------
// F3DDKR HLE renderer — untextured, vertex-shaded.
//
// gfxtask_run_xbus (src/rcp_dkr.c) hands us the display list the RSP would have
// run. This interprets the geometry half of the microcode the way the RSP does
// — matrix slots, vertex loads, billboarding, G_TRIN polygons — transforms to
// clip space, perspective divides, maps through the viewport, and hands the
// triangles to the host GL layer (linux/gfx.c), shaded from their vertex
// colours. Everything RDP-side (textures, combiners, blenders, rectangles) is
// ignored for now.
// ---------------------------------------------------------------------------

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define WINDOW_SCALE 3

#define GFX_MAX_VERTS 64    // the RSP's internal vertex array
#define GFX_MAX_DEPTH 16    // display-list recursion guard
#define GFX_MAX_TRI_VERTS 65536

// A vertex in clip space — post-matrix, pre-divide. Kept unprojected because
// near-plane clipping has to interpolate here, before the divide by w (a vertex
// behind the camera has w <= 0, and dividing by it is exactly the garbage the
// clip exists to prevent).
typedef struct {
    f32 clip[4];
    u8 r, g, b;
} GfxVertex;

// Clip anything closer than this. The N64 clips against w, and w is the
// camera-space depth, so this is the near plane in world units.
#define GFX_NEAR_W 1.0f

static u32 sGfxFrameCount = 0;

static f32 sMatrices[3][4][4]; // G_MTX_DKR_INDEX_0..2
static s32 sCurMatrix = 0;
static s32 sBillboard = FALSE;

static GfxVertex sVerts[GFX_MAX_VERTS];
static s32 sVertexBase = 0; // where G_VTX_APPEND vertices land

static f32 sVpScaleX = N64_SCREEN_W / 2.0f;
static f32 sVpScaleY = N64_SCREEN_H / 2.0f;
static f32 sVpTransX = N64_SCREEN_W / 2.0f;
static f32 sVpTransY = N64_SCREEN_H / 2.0f;

static GfxTriVert sTriVerts[GFX_MAX_TRI_VERTS];
static s32 sTriVertCount = 0;

/**
 * Unpack an N64 Mtx (s15.16 fixed point: the integer halves live in the first 8
 * words, the fractional halves in the last 8) into floats — the inverse of
 * mtxf_to_mtx() in src/hasm/math_util.c. Deliberately kept in the same format
 * the RSP consumes, so the PC build sees the same rounding and the same +-32768
 * saturation the console does and the N64 ROM stays a usable oracle.
 */
static void mtx_to_float(const Mtx *m, f32 out[4][4]) {
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
}

/**
 * Transform a vertex the way the RSP does: as a row vector times the selected
 * matrix (the same convention as mtxf_transform_point), then — when
 * billboarding — add the anchor's clip coordinates, then perspective divide and
 * map through the viewport.
 */
static void load_vertex(GfxVertex *dst, const Vertex *v, const f32 *anchor) {
    const f32 (*m)[4] = sMatrices[sCurMatrix];
    f32 x = v->x, y = v->y, z = v->z;
    s32 i;

    for (i = 0; i < 4; i++) {
        dst->clip[i] = (m[0][i] * x) + (m[1][i] * y) + (m[2][i] * z) + m[3][i];
        if (anchor != NULL) {
            dst->clip[i] += anchor[i];
        }
    }

    dst->r = v->r;
    dst->g = v->g;
    dst->b = v->b;
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

    for (i = 0; i < count && dstIdx + i < GFX_MAX_VERTS; i++) {
        // Billboarded vertices are sprite-space offsets from vertex 0, the
        // anchor pushed just before the billboard matrix.
        load_vertex(&sVerts[dstIdx + i], &src[i], sBillboard ? sVerts[0].clip : NULL);
    }

}

/**
 * Perspective divide plus the viewport map — the last thing the RSP does before
 * handing a vertex to the RDP. Only ever called on clipped vertices, so w is
 * guaranteed positive here.
 */
static void push_corner(const GfxVertex *v) {
    f32 invW = 1.0f / v->clip[3];

    sTriVerts[sTriVertCount].x = (v->clip[0] * invW * sVpScaleX) + sVpTransX;
    sTriVerts[sTriVertCount].y = sVpTransY - (v->clip[1] * invW * sVpScaleY);
    // Negated so nearer geometry gets the smaller depth under GL_LESS.
    sTriVerts[sTriVertCount].z = -(v->clip[2] * invW);
    sTriVerts[sTriVertCount].r = v->r;
    sTriVerts[sTriVertCount].g = v->g;
    sTriVerts[sTriVertCount].b = v->b;
    sTriVertCount++;
}

/**
 * Split the edge a->b where it crosses the near plane, at the point where
 * w == GFX_NEAR_W. Colour interpolates linearly in clip space along with the
 * position, which is what the RSP's own clipper does.
 */
static void clip_edge(const GfxVertex *a, const GfxVertex *b, GfxVertex *out) {
    f32 t = (GFX_NEAR_W - a->clip[3]) / (b->clip[3] - a->clip[3]);
    s32 i;

    for (i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + ((b->clip[i] - a->clip[i]) * t);
    }
    out->r = (u8) (a->r + ((f32) (b->r - a->r) * t));
    out->g = (u8) (a->g + ((f32) (b->g - a->g) * t));
    out->b = (u8) (a->b + ((f32) (b->b - a->b) * t));
}

/**
 * Clip a triangle against the near plane (Sutherland-Hodgman on the single
 * w >= GFX_NEAR_W plane), then fan-triangulate whatever polygon survives and
 * emit it. One clipped triangle yields 0, 1 or 2 output triangles.
 */
static void emit_triangle(const GfxVertex *v0, const GfxVertex *v1, const GfxVertex *v2) {
    const GfxVertex *in[3];
    GfxVertex poly[4];
    s32 numOut = 0;
    s32 i;

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
        if (sTriVertCount + 3 > GFX_MAX_TRI_VERTS) {
            return;
        }
        push_corner(&poly[0]);
        push_corner(&poly[i - 1]);
        push_corner(&poly[i]);
    }
}

/**
 * G_TRIN — DKR's polygon command. w1 points at an array of Triangles (vertex
 * indices plus UVs); shade each one from its vertex colours.
 */
static void handle_polygon(u32 w0, u32 w1) {
    const Triangle *tris = (const Triangle *) w1;
    s32 count = (((w0 >> 16) & 0xFF) >> 4) + 1;
    s32 i;

    if (tris == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        emit_triangle(&sVerts[tris[i].vi0 % GFX_MAX_VERTS], &sVerts[tris[i].vi1 % GFX_MAX_VERTS],
                      &sVerts[tris[i].vi2 % GFX_MAX_VERTS]);
    }
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
                    mtx_to_float((const Mtx *) w1, sMatrices[slot]);
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
                }
                break;
            }
            case G_MOVEMEM: {
                if ((w0 & 0xFF) == G_MV_VIEWPORT && w1 != 0) {
                    const Vp *vp = (const Vp *) w1;
                    sVpScaleX = vp->vp.vscale[0] / 4.0f;
                    sVpScaleY = vp->vp.vscale[1] / 4.0f;
                    sVpTransX = vp->vp.vtrans[0] / 4.0f;
                    sVpTransY = vp->vp.vtrans[1] / 4.0f;
                }
                break;
            }
            case G_VTX:
                handle_vertex(w0, w1);
                break;
            case G_TRIN:
                handle_polygon(w0, w1);
                break;
            case G_DL:
                run_dl((const Gfx *) w1, 0, depth + 1);
                if (((w0 >> 16) & 0xFF) == G_DL_NOPUSH) {
                    return; // a branch, not a call
                }
                break;
            case G_DMADL:
                run_dl((const Gfx *) w1, (w0 >> 16) & 0xFF, depth + 1);
                break;
            case (u8) G_ENDDL:
                return;
            default:
                // Everything else is RDP state (textures, combiners, blenders,
                // rectangles) — nothing for a wireframe to do.
                break;
        }
    }
}

void pc_gfx_task_submit(void *dlBegin, void *dlEnd) {
    sGfxFrameCount++;

    gfx_frame_begin();

    sCurMatrix = 0;
    sBillboard = FALSE;
    sVertexBase = 0;
    sTriVertCount = 0;
    run_dl((const Gfx *) dlBegin, (const Gfx *) dlEnd - (const Gfx *) dlBegin, 0);
    gfx_draw_tris(sTriVerts, sTriVertCount);

    gfx_frame_end();
}

int main(int argc, char **argv) {
    printf("=== DKR PC ===\n");
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
    gfx_window_init(N64_SCREEN_W, N64_SCREEN_H, WINDOW_SCALE);
    thread3_main(0);
    return 0;
}
