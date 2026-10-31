// The game's side of the native renderer: what the game sets and draws, called
// directly from its code (dreamcast/nat_gbi.h). Colours, matrices and modes are
// kept here as plain state. A draw reads them once and leaves a record with what it
// came to, in the frame's array; the records are drawn where the frame goes
// out (dreamcast/native.c), which has to be after the game's whole frame: a fade
// is known only at its end and goes into every colour before it, and a level load
// can drop a frame half made. Nothing is decoded on the way.
#include <stdio.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>
#include <kos.h>
#include "../src/textures_sprites.h"
#include "native.h"

NatState gNat;
NatRec gNatRecs[2][NAT_RECORDS] __attribute__((aligned(32)));
s32 gNatRecCount[2];

#define NAT_DEFERRED 64
static NatRec sDeferred[NAT_DEFERRED];
static s32 sDeferredCount, sDeferStart = -1;
static s32 sTask;
static s32 sIgnore;
static s32 sOverflow;

/** A frame starts over: as the walk began a list. */
void pc_nat_frame_reset(s32 task) {
    sTask = task;
    gNatRecCount[task] = 0;
    sDeferredCount = 0;
    sDeferStart = -1;
    gNat.cur = 0;
    gNat.billboard = FALSE;
    gNat.numVerts = gNat.numVerts2 = 0;
    gNat.otherL = 0;
    gNat.geometry = G_SHADE | G_SHADING_SMOOTH | G_ZBUFFER;
    gNat.texture.handle = 0;
    gNat.texture.header = NULL;
    gNat.lastHeader = NULL;
}

NatRec *pc_nat_record(s32 kind) {
    NatRec *rec;

    if (gNatRecCount[sTask] == NAT_RECORDS) {
        if (!sOverflow) {
            sOverflow = TRUE;
            printf("native: frame's records full (%d)\n", NAT_RECORDS);
        }
        return NULL;
    }
    rec = &gNatRecs[sTask][gNatRecCount[sTask]++];
    rec->kind = kind;
    rec->fog = (gNat.geometry & G_FOG) != 0;
    rec->prim = gNat.prim;
    rec->env = gNat.env;
    rec->matrix = gNat.matrix[gNat.cur];
    rec->matrix2 = gNat.matrix[2];
    return rec;
}

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
    }
}

void nat_tag(u32 tag) {
    NatRec *rec = pc_nat_record(NAT_REC_TAG);

    if (rec != NULL) {
        rec->b = tag;
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
    }
}

void nat_texrect(s32 xl, s32 yl, s32 xh, s32 yh, s32 s, s32 t, s32 dsdx, s32 dtdy) {
    const NatMaterial *mat = pc_nat_material_now(TRUE);
    NatRec *rec;

    if (mat != NULL && (rec = pc_nat_record(NAT_REC_RECT)) != NULL) {
        rec->mat = mat;
        rec->f[0] = xl / 4.0f;
        rec->f[1] = yl / 4.0f;
        rec->f[2] = xh / 4.0f;
        rec->f[3] = yh / 4.0f;
        rec->a = ((u32) s << 16) | ((u32) t & 0xFFFF);
        rec->b = ((u32) dsdx << 16) | ((u32) dtdy & 0xFFFF);
    }
}

void nat_fillrect(s32 ulx, s32 uly, s32 lrx, s32 lry) {
    const NatMaterial *mat = pc_nat_material_now(FALSE);
    NatRec *rec;

    if (mat != NULL && (rec = pc_nat_record(NAT_REC_FILL)) != NULL) {
        rec->mat = mat;
        rec->f[0] = ulx;
        rec->f[1] = uly;
        rec->f[2] = lrx;
        rec->f[3] = lry;
        rec->b = gNat.fill;
    }
}

void nat_defer_begin(void) {
    sDeferStart = gNatRecCount[sTask];
}

void nat_defer_end(void) {
    s32 i;

    if (sDeferStart < 0) {
        return;
    }
    for (i = sDeferStart; i < gNatRecCount[sTask] && sDeferredCount < NAT_DEFERRED; i++) {
        sDeferred[sDeferredCount++] = gNatRecs[sTask][i];
    }
    gNatRecCount[sTask] = sDeferStart;
    sDeferStart = -1;
}

void nat_defer_flush(void) {
    s32 i;

    for (i = 0; i < sDeferredCount; i++) {
        NatRec *rec = pc_nat_record(sDeferred[i].kind);

        if (rec != NULL) {
            *rec = sDeferred[i];
        }
    }
    sDeferredCount = 0;
}
