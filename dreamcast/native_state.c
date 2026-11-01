// The game's side of the native renderer: what the game sets and draws, called
// directly from its code (dreamcast/nat_gbi.h). Colours, matrices and modes are
// kept here as plain state. A draw reads them once, leaves a record with what it
// came to, and goes straight into the bin of its list: enDjinn's render list, which
// keeps it in the game's order among that list's draws. Nothing reaches the TA
// here: a list is run through when the scene goes out, after the game's whole frame
// (dreamcast/native.c), because a fade is known only at its end and goes into every
// colour before it, a level load can drop a frame half made, and textures are
// uploaded and FSAA is switched while the game runs.
//
// What holds for the draws after it, the viewport and the scissor and where a fade
// or the glare has been drawn, goes into every list that needs it, as a record of its
// own: each list is one pass over the frame, and keeps its place in that order.
// A batch without a depth test paints by the game's order alone, so where it goes is
// settled here, against what the frame has drawn so far: the record says so.
#include <stdio.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>
#include <kos.h>
#include <enDjinn/enj_render.h>
#include "../src/textures_sprites.h"
#include "cover.h"
#include "prof.h"
#include "scrawl.h"
#include "swirl.h"
#include "native.h"

NatState gNat;
static NatRec sRecs[NAT_RECORDS] __attribute__((aligned(32)));
static s32 sRecCount;

// Draws set aside, list by list, for nat_defer_flush.
#define NAT_DEFERRED 192
static struct {
    NatRec *rec;
    s32 list;
} sDeferred[NAT_DEFERRED];
static s32 sDeferredCount, sDeferring;
static s32 sIgnore;
static s32 sOverflow;

// Where the frame stands, in the game's order.
static s32 sSeenDepth;  // this view has drawn with a depth test: what has none is over it
static s32 sBlendQueued; // something is in the punch-through or translucent list
static s32 sCover;      // the game is drawing a cover (PC_COVER_*)
static s32 sCoverDrawn; // and a batch of it went to the opaque list
static s32 sCoverLayer; // covers finished this frame
static s32 sViewBackdrop; // the next polygon is a view's gradient
static s32 sSky;          // pc_nat_sky
static NatRec *sRectRun; // the run of rectangles the next may join

#if DKR_DEBUG_VIEW
// The frame's bins as they were filled, to queue them again while a frame is held.
#define NAT_REPLAY (NAT_RECORDS * 2)
static struct {
    NatRec *rec;
    s32 list;
} sReplay[NAT_REPLAY];
static s32 sReplayCount;
#endif

/** A frame starts over, and what was queued for one that was dropped is gone. */
void pc_nat_frame_reset(void) {
    enj_render_lists_clear();
    sRecCount = 0;
    sDeferredCount = 0;
    sDeferring = FALSE;
    sSeenDepth = FALSE;
    sBlendQueued = FALSE;
    sCover = sCoverDrawn = sCoverLayer = 0;
    sViewBackdrop = FALSE;
    sRectRun = NULL;
#if DKR_DEBUG_VIEW
    sReplayCount = 0;
#endif
    gNat.cur = 0;
    gNat.billboard = FALSE;
    gNat.numVerts = gNat.numVerts2 = 0;
    gNat.otherL = 0;
    gNat.geometry = G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER;
    gNat.texture.handle = 0;
    gNat.texture.header = NULL;
    gNat.lastHeader = NULL;
    pc_nat_planes_forget();
}

NatRec *pc_nat_record(s32 kind) {
    NatRec *rec;

    if (sRecCount == NAT_RECORDS) {
        if (!sOverflow) {
            sOverflow = TRUE;
            printf("native: frame's records full (%d)\n", NAT_RECORDS);
        }
        return NULL;
    }
    rec = &sRecs[sRecCount++];
    rec->kind = kind;
    rec->fog = (gNat.geometry & G_FOG) != 0;
    rec->place = GFX_PLACE_NONE;
    rec->layer = 0;
    rec->prim = gNat.prim;
    rec->env = gNat.env;
    rec->matrix = gNat.matrix[gNat.cur];
    rec->matrix2 = gNat.matrix[2];
    return rec;
}

static void nat_queue(s32 list, NatRec *rec) {
    static const pvr_list_t sLists[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };

    enj_render_list_add(sLists[list], pc_nat_item, rec);
#if DKR_DEBUG_VIEW
    if (sReplayCount < NAT_REPLAY) {
        sReplay[sReplayCount].rec = rec;
        sReplay[sReplayCount++].list = list;
    }
#endif
}

static void nat_bin(s32 list, NatRec *rec) {
    if (!sDeferring) {
        nat_queue(list, rec);
    } else if (sDeferredCount < NAT_DEFERRED) {
        sDeferred[sDeferredCount].rec = rec;
        sDeferred[sDeferredCount++].list = list;
    }
}

void pc_nat_bin(s32 list, NatRec *rec) {
    if (list != NAT_LIST_OP) {
        sBlendQueued = TRUE;
    }
    nat_bin(list, rec);
}

/** What goes for the draws after it, in every list. */
static void nat_bin_all(NatRec *rec) {
    nat_bin(NAT_LIST_OP, rec);
    nat_bin(NAT_LIST_PT, rec);
    nat_bin(NAT_LIST_TR, rec);
}

void pc_nat_depth_drawn(void) {
    sSeenDepth = TRUE;
}

/**
 * Whether this polygon is the gradient a view opens with, which is opaque. It has to
 * be said: the game writes those vertices only after the draw, as the N64 reads
 * them later.
 */
s32 pc_nat_view_backdrop(void) {
    s32 backdrop = sViewBackdrop;

    sViewBackdrop = FALSE;
    return backdrop;
}

s32 pc_nat_may_open(void) {
    return (!sSeenDepth && (!sBlendQueued || sSky)) || sCover;
}

/**
 * The sky dome is being drawn. Its pieces paint by order and none has a depth test,
 * and a blended sun among them must not send the opaque pieces after it to the
 * translucent list, where they came out as the level's background fill.
 */
void pc_nat_sky(s32 sky) {
    sSky = sky;
}

/**
 * A draw without a depth test. With nothing yet that it could cover, an opaque one
 * may go to the opaque list, which keeps its order: that is the backdrop (screen
 * fill, sky). A cover goes there as well. Anything else goes to the translucent
 * list in order, which is what keeps the HUD on top. `alpha` is what the batch
 * emits and `handle` its texture, both asked for only when pc_nat_may_open.
 */
void pc_nat_bin_2d(NatRec *rec, s32 alphaTest, s32 alpha, u32 handle) {
    s32 list = NAT_LIST_TR;

    rec->layer = sCoverLayer;
    if (alphaTest) {
        list = NAT_LIST_PT;
        rec->place = GFX_PLACE_OVERLAY; // alpha-tested 2D, drawn after OP like TR
    } else if (!sSeenDepth && (!sBlendQueued || sSky) && alpha == 0xFF && (handle == 0 || gfx_texture_opaque(handle))) {
        list = NAT_LIST_OP;
    } else if (sCover && alpha == 0xFF && handle == 0) {
        list = NAT_LIST_OP;
        rec->place = GFX_PLACE_COVER;
        sCoverDrawn = TRUE;
    } else {
        rec->place = sSeenDepth ? GFX_PLACE_OVERLAY : GFX_PLACE_BACKDROP;
    }
    pc_nat_bin(list, rec);
}

#if DKR_DEBUG_VIEW
void pc_nat_frame_requeue(void) {
    s32 i, count = sReplayCount;

    for (i = 0; i < count; i++) {
        nat_queue(sReplay[i].list, sReplay[i].rec);
    }
    sReplayCount = count;
}
#endif

void nat_ignore(s32 ignore) {
    sIgnore = ignore;
}

void nat_color(u32 cmd, u32 rgba) {
    NatRec *rec;

    switch (cmd) {
        case (u8) G_SETPRIMCOLOR:
            gNat.prim = rgba;
            break;
        case (u8) G_SETENVCOLOR:
            gNat.env = rgba;
            break;
        case (u8) G_SETBLENDCOLOR:
            gNat.blend = rgba;
            break;
        case (u8) G_SETFILLCOLOR:
            gNat.fill = rgba;
            break;
        case (u8) G_SETFOGCOLOR:
            rec = pc_nat_record(NAT_REC_FOG_COLOUR);
            if (rec != NULL) {
                rec->b = rgba;
                nat_bin(NAT_LIST_OP, rec); // the PVR has one fog table a frame: the last one holds
            }
            break;
        default:
            break;
    }
}

void nat_combine(u32 w0, u32 w1) {
    gNat.combine0 = w0;
    gNat.combine1 = w1;
}

void nat_other(u32 high, u32 shift, u32 length, u32 data) {
    u32 mask = (length >= 32) ? 0xFFFFFFFF : (((u32) 1 << length) - 1) << shift;

    if (high) {
        gNat.otherH = (gNat.otherH & ~mask) | (data & mask);
    } else {
        gNat.otherL = (gNat.otherL & ~mask) | (data & mask);
    }
}

void nat_other_both(u32 high, u32 low) {
    gNat.otherH = high & 0x00FFFFFF;
    gNat.otherL = low;
}

void nat_geometry(u32 set, u32 clear) {
    gNat.geometry = (gNat.geometry | set) & ~clear;
}

void nat_moveword(u32 index, u32 data) {
    NatRec *rec;

    if (index == G_MW_MVPMATRIX) {
        gNat.cur = (data >> 6) & 3;
    } else if (index == G_MW_BILLBOARD) {
        gNat.billboard = data != 0;
    } else if (index == G_MW_FOG) {
        rec = pc_nat_record(NAT_REC_FOG_DEPTH);
        if (rec != NULL) {
            rec->b = data;
            nat_bin(NAT_LIST_OP, rec);
        }
    }
}

void nat_matrix(const void *matrix, u32 slot) {
    if (matrix != NULL && slot < 3) {
        gNat.matrix[slot] = matrix;
        gNat.cur = slot;
    }
}

void nat_viewport(const void *viewport) {
    NatRec *rec;

    if (viewport != NULL) {
        rec = pc_nat_record(NAT_REC_VIEWPORT);
        if (rec != NULL) {
            rec->p0 = viewport;
            nat_bin_all(rec);
        }
    }
}

void nat_scissor(f32 x0, f32 y0, f32 x1, f32 y1) {
    NatRec *rec = pc_nat_record(NAT_REC_SCISSOR);

    // Lower-right inclusive, as the RDP has it.
    if (rec != NULL) {
        rec->f[0] = x0;
        rec->f[1] = y0;
        rec->f[2] = x1 + 1.0f;
        rec->f[3] = y1 + 1.0f;
        nat_bin_all(rec);
    }
}

void nat_tag(u32 tag) {
    NatRec *rec;

    sRectRun = NULL;
    if (tag == PC_COVER_BEGIN_TAG || tag == PC_COVER_END_TAG) {
        if (tag == PC_COVER_END_TAG && sCover && sCoverDrawn) {
            sCoverLayer++; // 2D drawn from here on goes over it
        }
        sCover = tag == PC_COVER_BEGIN_TAG;
        sCoverDrawn = FALSE;
        return;
    }
    if (tag == PC_VIEW_BEGIN_TAG) {
        // Depth seen is a view's, not the frame's: in split screen the second view's
        // backdrop comes after the first view's scene, and taken for 2D it went over
        // its own view.
        sSeenDepth = FALSE;
        sViewBackdrop = TRUE;
        return;
    }
    rec = pc_nat_record(NAT_REC_TAG);
    if (rec == NULL) {
        return;
    }
    rec->b = tag;
    if (tag == PC_SCRAWL_TAG) {
        pc_nat_bin(NAT_LIST_TR, rec);
    } else if (tag == PC_SWIRL_TAG) {
        // Opaque or not as it fades, and its halo blended either way.
        pc_nat_bin(NAT_LIST_OP, rec);
        pc_nat_bin(NAT_LIST_TR, rec);
    } else {
        nat_bin_all(rec);
    }
}

void nat_vertices(const void *verts, s32 count, s32 append) {
    if (append) {
        gNat.verts2 = verts;
        gNat.numVerts2 = count;
        gNat.matrixV2 = gNat.matrix[gNat.cur];
    } else {
        gNat.verts = verts;
        gNat.numVerts = count;
        gNat.numVerts2 = 0;
        gNat.matrixV = gNat.matrix[gNat.cur];
    }
}

/** The texture a list of the game's would have loaded. */
static void nat_set_texture(const TextureHeader *tex) {
    if (tex != gNat.texture.header) {
        pc_nat_texture_of(tex, &gNat.texture);
    }
}

void nat_image(const void *image, s32 fmt, s32 siz, s32 width, s32 height, s32 cms, s32 cmt, s32 swapped) {
    if (!sIgnore) {
        pc_nat_texture_image(image, fmt, siz, width, height, cms, cmt, swapped, &gNat.texture);
    }
}

void nat_table(const void *entry, s32 count) {
    const Gfx *g = entry;
    s32 i;

    if (sIgnore) {
        return;
    }
    if (count == 0) {
        nat_set_texture((const TextureHeader *) entry);
        return;
    }
    // A render table's entry, read as data: a combiner and the modes.
    for (i = 0; i < count; i++) {
        u32 op = (g[i].words.w0 >> 24) & 0xFF;

        if (op == (u8) G_SETCOMBINE) {
            nat_combine(g[i].words.w0, g[i].words.w1);
        } else if (op == (u8) G_RDPSETOTHERMODE) {
            nat_other_both(g[i].words.w0, g[i].words.w1);
        } else if (op == (u8) G_SETOTHERMODE_H || op == (u8) G_SETOTHERMODE_L) {
            nat_other(op == (u8) G_SETOTHERMODE_H, (g[i].words.w0 >> 8) & 0xFF, g[i].words.w0 & 0xFF, g[i].words.w1);
        }
    }
}

void nat_polygon(const void *tris, s32 count, s32 textured) {
    NatRec *rec;
    const NatMaterial *mat;

    if (sIgnore || tris == NULL || gNat.numVerts == 0) {
        return;
    }
    mat = pc_nat_material_now(textured);
    if (mat == NULL) {
        return;
    }
    rec = pc_nat_record(NAT_REC_TRIS);
    if (rec != NULL) {
        rec->mat = mat;
        rec->matrix = gNat.matrixV;
        rec->matrix2 = gNat.matrixV2;
        rec->p0 = gNat.verts;
        rec->p1 = gNat.verts2;
        rec->p2 = tris;
        rec->a = gNat.numVerts;
        rec->b = gNat.numVerts2;
        rec->c = count;
        pc_nat_bin_tris(rec);
    }
}

void nat_texrect(s32 xl, s32 yl, s32 xh, s32 yh, s32 s, s32 t, s32 dsdx, s32 dtdy) {
    const NatMaterial *mat = pc_nat_material_now(TRUE);
    u32 handle;
    NatRec *rec;

    if (mat == NULL || (handle = pc_nat_material_handle(mat)) == 0 || (rec = pc_nat_record(NAT_REC_RECT)) == NULL) {
        return;
    }
    rec->mat = mat;
    rec->f[0] = xl / 4.0f;
    rec->f[1] = yl / 4.0f;
    rec->f[2] = xh / 4.0f;
    rec->f[3] = yh / 4.0f;
    rec->a = ((u32) s << 16) | ((u32) t & 0xFFFF);
    rec->b = ((u32) dsdx << 16) | ((u32) dtdy & 0xFFFF);
    // Text, mostly: a run under one material and colour with nothing between its
    // rectangles goes as one item.
    if (sRectRun != NULL && sRectRun + sRectRun->run == rec && sRectRun->run < NAT_RECT_RUN && sRectRun->mat == mat &&
        sRectRun->prim == rec->prim && sRectRun->env == rec->env) {
        sRectRun->run++;
        return;
    }
    rec->run = 1;
    sRectRun = rec;
    pc_nat_bin_2d(rec, FALSE, pc_nat_may_open() ? pc_nat_rect_alpha(rec) : 0, handle);
}

void nat_fillrect(s32 ulx, s32 uly, s32 lrx, s32 lry) {
    const NatMaterial *mat = pc_nat_material_now(FALSE);
    s32 alpha = 0;
    NatRec *rec;

    if (mat != NULL && (rec = pc_nat_record(NAT_REC_FILL)) != NULL) {
        rec->mat = mat;
        rec->f[0] = ulx;
        rec->f[1] = uly;
        rec->f[2] = lrx;
        rec->f[3] = lry;
        rec->b = gNat.fill;
        if (pc_nat_may_open()) {
            alpha = pc_nat_rect_alpha(rec);
            if (alpha == 0) {
                return; // draws nothing, and so does not count as drawn
            }
        }
        pc_nat_bin_2d(rec, FALSE, alpha, 0);
    }
}

void nat_defer_begin(void) {
    sDeferring = TRUE;
    sRectRun = NULL;
}

void nat_defer_end(void) {
    sDeferring = FALSE;
    sRectRun = NULL;
}

void nat_defer_flush(void) {
    s32 i;

    for (i = 0; i < sDeferredCount; i++) {
        nat_queue(sDeferred[i].list, sDeferred[i].rec);
    }
    sDeferredCount = 0;
    sRectRun = NULL;
}
