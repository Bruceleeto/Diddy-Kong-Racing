// The native renderer. What the display-list walk worked out on every frame,
// batch after batch, build_native_meshes.py has worked out once: here a chunk
// (a batch of the game's data) is a bounding box, positions, a run of corners
// ready for the PVR and a compiled header. A frame culls the chunk, transforms
// its positions, sends the header if it is not the last one sent, and writes the
// strips.
//
// The chunk loop and the two writers are the frame's hot code, and the instruction
// cache is 8 KB and direct-mapped: they stay small, and have no branch on what
// a chunk is. Anything out of the ordinary (a corner behind the near plane, a
// fade drawn into the frame, a texture that animates) has its own path out of line.
#include "dkr_asset_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <ultra64.h>
#include <structs.h>
#include <f3ddkr.h>
#include <kos.h>
#include "../src/textures_sprites.h"
#include "native.h"

#ifndef DKR_PROF_SKIP
#define DKR_PROF_SKIP 0
#endif

#define NAT_MAX_POSITIONS 64
#define NAT_MAX_CORNERS 64 // sixteen triangles at most in a batch
#define NAT_CHUNK_WATER 1
#define NAT_CHUNK_TEX_ANIM 2
#define NAT_CHUNK_DECAL 16
#define NAT_CHUNK_TEXTURED 32
#define NAT_CHUNK_ROUTE(flags) (((flags) >> 6) & 3)
#define NAT_BATCH_WALK 0xFFFF
#define NAT_BATCH_NONE 0xFFFE
#define NAT_SCROLL_TURNS 8.0f
// A combiner's rule (dreamcast/build_native_meshes.py, CC_RULE_*).
#define NAT_CC_RULE 3
#define NAT_CC_SHADE 1
#define NAT_CC_WHITE 2

struct NatMaterial {
    pvr_poly_hdr_t hdr; // no culling: these go out as triangles, culled here
    f32 uMul, vMul;
    u32 combine0, combine1, cycle;
    u32 rule; // NAT_CC_*
    u32 handle;
    u32 otherL, otherH;
    u16 width, height;
    u8 cms, cmt;
    u8 route, decal, textured, geometry, depth, depthWrite, blend, alphaTest, fog, point;
    u32 used;
} __attribute__((aligned(32)));

// A level's block, as the pack holds it (dreamcast/build_native_meshes.py).
typedef struct {
    char magic[4];
    u32 modelSize, numMats, numSegs, numChunks;
    u32 matsAt, infosAt, segsAt, chunksAt, xyzAt, cornersAt;
} NatLevelHeader;

typedef struct {
    u8 route, decal;
    s16 texture;
    u16 frame, combiner;           // combiner: its number in the pack times 4, plus its rule
    u32 combine0, combine1, cycle; // what the game's combiner would be set to
} NatMatInfo;

typedef struct {
    u32 tableAt; // u16 per batch: its chunk, or NAT_BATCH_*
    u16 batches, opaqueBatches;
} NatSegment;

typedef struct {
    f32 centre[3], half[3];
    union {
        u32 xyzAt;
        const f32 *xyz;
    };
    union {
        u32 cornersAt;
        const NatCorner *corners;
    };
    u16 numPositions, oneSided, twoSided, matOne, matTwo, flags, batch, unused;
} NatChunk;

_Static_assert(sizeof(NatChunk) == 48, "NatChunk is what the pack holds");
_Static_assert(sizeof(NatCorner) == 16, "NatCorner is what the pack holds");

// The level the game has loaded.
static NatLevelHeader *sLevel;
static pvr_poly_hdr_t *sMats;
static const NatMatInfo *sInfos;
static const NatSegment *sSegs;
static NatChunk *sChunks;
static s32 sResolved; // the mats have their textures' words, for video memory as it is now
static u8 sLevelWhole; // every mat resolved
// An animated texture's frames, as header words: per level texture, NULL if it has none.
static u32 **sFrameWords;
static u8 *sMatGood;
static s32 sVramGeneration = 1; // a model's mats are good for one of these

// A frame's five planes through the view: the box is outside when its corner
// nearest a plane's positive side lies behind it.
typedef struct {
    f32 n[3], d;
    f32 abs[3], pad;
} NatPlane;

static DKR_HOT NatPlane sPlanes[5];
static DKR_HOT const f32 *sPlanesFor; // the matrix they are of; NULL when a frame starts
static DKR_HOT s32 sListRoute;        // the list that is open, as the pack has it

// A position as a frame leaves it, and for an object model its colour.
typedef struct {
    f32 x, y, z;
    u32 argb;
} NatPoint;

static DKR_HOT NatPoint sPoints[NAT_MAX_POSITIONS] __attribute__((aligned(32)));

extern LevelModel *gCurrentLevelModel;


void pc_nat_level_free(void) {
    if (sFrameWords != NULL && sLevel != NULL) {
        s32 i;

        for (i = 0; i < gCurrentLevelModel->numberOfTextures; i++) {
            free(sFrameWords[i]);
        }
    }
    free(sFrameWords);
    free(sMatGood);
    free(sLevel);
    sFrameWords = NULL;
    sMatGood = NULL;
    sLevel = NULL;
    sResolved = FALSE;
}

// The pack's index: level models, then object models.
static file_t sPack = FILEHND_INVALID;
static u32 *sPackIndex;
static u32 sPackLevels, sPackEntries;

static s32 nat_pack_open(void) {
    u32 head[4];

    if (sPack != FILEHND_INVALID) {
        return TRUE;
    }
    if (sPackIndex != NULL) {
        return FALSE; // tried, and it did not work
    }
    sPack = fs_open(DKR_ASSET_MOUNT "/build/dc/native.pack", O_RDONLY);
    if (sPack == FILEHND_INVALID) {
        printf("native: no mesh pack\n");
        return FALSE;
    }
    if (fs_read(sPack, head, 16) != 16 || head[0] != 0x4B504E44 /* DNPK */ || head[1] != 3) {
        printf("native: mesh pack is not version 3\n");
        fs_close(sPack);
        sPack = FILEHND_INVALID;
        return FALSE;
    }
    sPackLevels = head[2];
    sPackEntries = head[2] + head[3];
    sPackIndex = malloc(sPackEntries * 8);
    if (sPackIndex == NULL || fs_read(sPack, sPackIndex, sPackEntries * 8) != (ssize_t) (sPackEntries * 8)) {
        printf("native: mesh pack index unread\n");
        sPackEntries = sPackLevels = 0;
        return FALSE;
    }
    return TRUE;
}

/** An entry of the pack in fresh memory, or NULL if it has none. */
static u8 *nat_pack_read(u32 entry) {
    u8 *block;
    u32 bytes;

    if (!nat_pack_open() || entry >= sPackEntries || sPackIndex[entry * 2 + 1] == 0) {
        return NULL;
    }
    bytes = sPackIndex[entry * 2 + 1];
    block = memalign(32, bytes);
    if (block == NULL) {
        printf("native: malloc failed, %lu bytes for entry %lu\n", (unsigned long) bytes, (unsigned long) entry);
        return NULL;
    }
    if (fs_seek(sPack, sPackIndex[entry * 2], SEEK_SET) < 0 || fs_read(sPack, block, bytes) != (ssize_t) bytes) {
        printf("native: entry %lu unread\n", (unsigned long) entry);
        free(block);
        return NULL;
    }
    return block;
}

/** Reads the level's block from the pack. Without it the walk draws the level, as it can. */
void pc_nat_level_load(s32 modelId) {
    u8 *block;
    u32 i;

    pc_nat_level_free();
    pc_nat_materials_forget();
    block = nat_pack_read(modelId);
    if (block == NULL) {
        return;
    }
    sLevel = (NatLevelHeader *) block;
    if (__builtin_memcmp(sLevel->magic, "DNML", 4) != 0 || sLevel->modelSize != (u32) gCurrentLevelModel->modelSize) {
        printf("native: level model %ld is not the one in the pack\n", (long) modelId);
        free(block);
        sLevel = NULL;
        return;
    }
    sMats = (pvr_poly_hdr_t *) (block + sLevel->matsAt);
    sInfos = (const NatMatInfo *) (block + sLevel->infosAt);
    sSegs = (const NatSegment *) (block + sLevel->segsAt);
    sChunks = (NatChunk *) (block + sLevel->chunksAt);
    for (i = 0; i < sLevel->numChunks; i++) {
        sChunks[i].xyz = (const f32 *) (block + sChunks[i].xyzAt);
        sChunks[i].corners = (const NatCorner *) (block + sChunks[i].cornersAt);
    }
    sMatGood = malloc(sLevel->numMats);
    sFrameWords = calloc(gCurrentLevelModel->numberOfTextures, sizeof(*sFrameWords));
    if (sMatGood == NULL || sFrameWords == NULL) {
        printf("native: malloc failed for level model %ld's tables\n", (long) modelId);
        pc_nat_level_free();
        return;
    }
    sResolved = FALSE;
}

/** Video memory was rebuilt: every address a mat holds is gone. */
void pc_nat_vram_lost(void) {
    sResolved = FALSE;
    sVramGeneration++;
    pc_nat_materials_forget();
}

/**
 * Fills in what only the console knows: where each texture went. The first
 * draw of a scene comes here, once its textures are all in video memory.
 */
static __attribute__((noinline, cold)) void nat_level_resolve(void) {
    u32 i;

    sResolved = TRUE;
    sLevelWhole = TRUE;
    for (i = 0; i < sLevel->numMats; i++) {
        const NatMatInfo *info = &sInfos[i];
        u32 mode2, mode3;

        sMatGood[i] = TRUE;
        if (info->texture < 0) {
            continue;
        }
        {
            TextureHeader *tex = gCurrentLevelModel->textures[info->texture].texture;

            tex = (TextureHeader *) ((u8 *) tex + info->frame * tex->textureSize);
            if (!pc_nat_texture_words(tex, &mode2, &mode3)) {
                printf("native: mat %lu: texture %ld frame %u (%ux%u fmt %u) is not in video memory\n", (unsigned long) i,
                       (long) info->texture, info->frame, tex->width, tex->height, tex->format & 0xF);
                sMatGood[i] = FALSE;
                sLevelWhole = FALSE;
                continue;
            }
            sMats[i].mode2 = (sMats[i].mode2 & ~(PVR_TA_PM2_USIZE_MASK | PVR_TA_PM2_VSIZE_MASK)) | mode2;
            sMats[i].mode3 = mode3;
        }
    }
    for (i = 0; i < sLevel->numChunks; i++) {
        const NatChunk *chunk = &sChunks[i];

        if ((chunk->flags & NAT_CHUNK_TEX_ANIM) && sInfos[chunk->matOne].texture >= 0) {
            s32 t = sInfos[chunk->matOne].texture;
            TextureHeader *tex = gCurrentLevelModel->textures[t].texture;
            s32 frames = tex->numOfTextures >> 8;
            s32 f;

            if (sFrameWords[t] == NULL) {
                sFrameWords[t] = malloc(frames * sizeof(u32));
            }
            if (sFrameWords[t] == NULL) {
                continue;
            }
            for (f = 0; f < frames; f++) {
                u32 mode2;

                if (!pc_nat_texture_words((u8 *) tex + f * tex->textureSize, &mode2, &sFrameWords[t][f])) {
                    sFrameWords[t][f] = sMats[chunk->matOne].mode3;
                }
            }
        }
    }
}

static void nat_level_record(s32 segment, s32 waterHidden, s32 first, s32 end, s32 route) {
    NatRec *rec = pc_nat_record(NAT_REC_LEVEL);

    if (rec != NULL) {
        rec->a = segment | (waterHidden ? 0x10000 : 0);
        rec->b = first;
        rec->c = end;
        pc_nat_depth_drawn();
        pc_nat_bin(route, rec);
    }
}

s32 pc_nat_level_batches(s32 segment, s32 batch, s32 end, s32 waterHidden) {
    const NatSegment *seg;
    const u16 *table;
    s32 next;
    s32 first = -1, last = -1, listRoute = NAT_LIST_OP;

    if (sLevel == NULL) {
        return 0;
    }
    if (!sResolved) {
        nat_level_resolve();
    }
    seg = &sSegs[segment];
    table = (const u16 *) ((const u8 *) sLevel + seg->tableAt);
    if (table[batch] == NAT_BATCH_WALK) {
        return 0;
    }
    for (next = batch; next < end; next++) {
        u16 chunk = table[next];

        if (chunk == NAT_BATCH_WALK) {
            break;
        }
        if (chunk != NAT_BATCH_NONE) {
            s32 route;

            if (!sMatGood[sChunks[chunk].matOne]) {
                if (next == batch) {
                    return 0;
                }
                break;
            }
            // A stretch of one list is a record; the batches keep their order in it.
            route = NAT_CHUNK_ROUTE(sChunks[chunk].flags);
            if (first >= 0 && route != listRoute) {
                nat_level_record(segment, waterHidden, first, last + 1, listRoute);
                first = -1;
            }
            if (first < 0) {
                first = chunk;
                listRoute = route;
            }
            last = chunk;
        }
    }
    if (first >= 0) {
        nat_level_record(segment, waterHidden, first, last + 1, listRoute);
    }
    return next - batch;
}

/**
 * A batch's texture moves under its surface: a waterfall, a fountain. The game has
 * added this to the batch's triangles (obj_loop_texscroll, obj_loop_animator), and it
 * goes on the chunk's corners the same way, in the block, so a frame draws them as
 * they stand and no draw pays for it. A new level is a new block, so it starts unmoved.
 * The game leaves out triangles with TRI_FLAG_80, but no scrolled batch has any. A
 * scrolled texture repeats, so its size is a power of two and these are exact, and
 * eight turns off is, as in the game, the same picture.
 */
__attribute__((noinline, cold)) void pc_nat_batch_scroll(s32 segment, s32 batch, s32 du, s32 dv) {
    const u16 *table;
    const NatChunk *chunk;
    const TextureHeader *tex;
    NatCorner *c, *end;
    f32 u, v, wrapU, wrapV;

    if (sLevel == NULL || (du | dv) == 0) {
        return;
    }
    table = (const u16 *) ((const u8 *) sLevel + sSegs[segment].tableAt);
    if (table[batch] >= NAT_BATCH_NONE) {
        return; // the walk draws what the game moved
    }
    chunk = &sChunks[table[batch]];
    c = (NatCorner *) chunk->corners;
    end = c + chunk->oneSided + chunk->twoSided;
    tex = gCurrentLevelModel->textures[sInfos[chunk->matOne].texture].texture;
    u = du / (32.0f * tex->width);
    v = dv / (32.0f * tex->height);
    wrapU = c->u > NAT_SCROLL_TURNS ? -NAT_SCROLL_TURNS : c->u < -NAT_SCROLL_TURNS ? NAT_SCROLL_TURNS : 0.0f;
    wrapV = c->v > NAT_SCROLL_TURNS ? -NAT_SCROLL_TURNS : c->v < -NAT_SCROLL_TURNS ? NAT_SCROLL_TURNS : 0.0f;
    u += wrapU;
    v += wrapV;
    for (; c < end; c++) {
        c->u += u;
        c->v += v;
    }
}

void pc_nat_planes_forget(void) {
    sPlanesFor = NULL;
}

static __attribute__((noinline)) void nat_planes(const f32 *matrix) {
    const f32 (*m)[4] = (const f32 (*)[4]) matrix;
    static const s8 sSide[5][2] = { { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { 3, 0 } };
    s32 p, a;

    for (p = 0; p < 5; p++) {
        s32 axis = sSide[p][0];
        f32 sign = sSide[p][1];

        for (a = 0; a < 4; a++) {
            f32 v = (p == 4) ? m[a][3] : m[a][3] + (sign * m[a][axis]);

            if (a < 3) {
                sPlanes[p].n[a] = v;
                sPlanes[p].abs[a] = v < 0.0f ? -v : v;
            } else {
                sPlanes[p].d = v;
            }
        }
    }
    sPlanesFor = matrix;
}

/** Whether the chunk's box lies wholly behind one of the view's planes. */
static inline s32 nat_outside(const NatChunk *chunk) {
    const NatPlane *p = sPlanes;
    s32 i;

    for (i = 0; i < 5; i++, p++) {
        f32 at = (p->n[0] * chunk->centre[0]) + (p->n[1] * chunk->centre[1]) + (p->n[2] * chunk->centre[2]) + p->d;
        f32 reach = (p->abs[0] * chunk->half[0]) + (p->abs[1] * chunk->half[1]) + (p->abs[2] * chunk->half[2]);

        if (at + reach < 0.0f) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * The positions into sPoints, framebuffer x and y and the depth. A position
 * behind the near plane sets the result, and the caller takes the slow way: the
 * loop itself does not branch on it.
 */
static inline s32 nat_transform(const f32 *xyz, s32 count, f32 sx, f32 sy, f32 tx, f32 ty, f32 zMul) {
    NatPoint *out = sPoints;
    const f32 *end = xyz + (count * 3);
    s32 behind = 0;

    for (; xyz + 3 < end; xyz += 6, out += 2) {
        shz_vec4_t c0 = shz_xmtrx_transform_vec4(shz_vec4_init(xyz[0], xyz[1], xyz[2], 1.0f));
        shz_vec4_t c1 = nat_transform_fv4(xyz[3], xyz[4], xyz[5]);
        f32 inv0 = shz_invf_fsrra(c0.w);
        f32 inv1 = shz_invf_fsrra(c1.w);

        behind |= (c0.w < 1.0f) | (c1.w < 1.0f);
        out[0].x = (c0.x * inv0 * sx) + tx;
        out[0].y = ty - (c0.y * inv0 * sy);
        out[0].z = inv0 * zMul;
        out[1].x = (c1.x * inv1 * sx) + tx;
        out[1].y = ty - (c1.y * inv1 * sy);
        out[1].z = inv1 * zMul;
    }
    if (xyz < end) {
        shz_vec4_t c = shz_xmtrx_transform_vec4(shz_vec4_init(xyz[0], xyz[1], xyz[2], 1.0f));
        f32 inv = shz_invf_fsrra(c.w);

        behind |= c.w < 1.0f;
        out->x = (c.x * inv * sx) + tx;
        out->y = ty - (c.y * inv * sy);
        out->z = inv * zMul;
    }
    return behind;
}

/** Writes strips to the opaque list. */
static inline void nat_write_sq(const NatCorner *c, s32 count, u32 offset, s32 lit) {
    u32 sq = pvr_dr_addr;
    const NatCorner *end = c + count;

    for (; c < end; c++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];
        pvr_vertex_t *vt;

        sq ^= 32;
        vt = (pvr_vertex_t *) sq;
        nat_vertex_put(vt, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, lit ? p->argb : c->argb, offset);
        nat_sq_flush(vt);
    }
    pvr_dr_addr = sq;
}

/** The same, with a fade drawn into the frame on the colours (dreamcast/cover.h). */
static __attribute__((noinline, cold)) void nat_write_faded(const NatCorner *c, s32 count, u32 offset, s32 textured) {
    u32 sq = pvr_dr_addr;
    s32 i;

    for (i = 0; i < count; i++, c++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];
        pvr_vertex_t *vt;

        sq ^= 32;
        vt = (pvr_vertex_t *) sq;
        nat_vertex_put(vt, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, gfx_frame_argb(c->argb, textured), offset);
        nat_sq_flush(vt);
    }
    pvr_dr_addr = sq;
}

/** Strips with a position behind the near plane, whose positions are compiled: their matrix is in XMTRX. */
static __attribute__((noinline, cold)) void nat_clip_level(const NatChunk *chunk, const NatCorner *corners, s32 count, s32 cull,
                                                           f32 scaleX, f32 scaleY, f32 zMul, u32 offset, s32 textured) {
    shz_vec4_t clip[NAT_MAX_POSITIONS];
    s32 i;

    for (i = 0; i < chunk->numPositions; i++) {
        const f32 *p = &chunk->xyz[i * 3];

        clip[i] = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1.0f));
    }
    pc_nat_clip(corners, count, clip, NULL, cull, scaleX, scaleY, zMul, offset, textured);
}

/**
 * Strips under a scissor that is not on the PVR's tiles, which it would overdraw
 * by up to a tile: split-screen-like views and menus. Triangle by triangle
 * through the scissor.
 */
static __attribute__((noinline, cold)) void nat_write_exact(const NatCorner *corners, s32 count, u32 offset, s32 lit,
                                                            s32 textured, f32 zMul, f32 scaleX, f32 scaleY) {
    GfxTriVert t[3];
    s32 step = 0;
    s32 i;

    for (i = 0; i < count; i++) {
        const NatCorner *c = &corners[i];
        const NatPoint *p = &sPoints[c->word & 0xFFFF];
        GfxTriVert *out = &t[step < 3 ? step : 2];
        u32 argb;

        if (step >= 3) {
            t[0] = t[1];
            t[1] = t[2];
        }
        argb = lit ? p->argb : (gDcFadeOn ? gfx_frame_argb(c->argb, textured) : c->argb);
        out->x = p->x / scaleX;
        out->y = p->y / scaleY;
        out->w = zMul / p->z;
        out->z = 0.0f;
        out->u = c->u;
        out->v = c->v;
        out->fog = 0.0f;
        out->a = argb >> 24;
        out->r = argb >> 16;
        out->g = argb >> 8;
        out->b = argb;
        step++;
        if (step >= 3) {
            if (step & 1) {
                gfx_nat_tri_exact(t, zMul, offset);
            } else {
                GfxTriVert swapped[3] = { t[1], t[0], t[2] };

                gfx_nat_tri_exact(swapped, zMul, offset);
            }
        }
        if ((c->word & 0xF0000000u) == PVR_CMD_VERTEX_EOL) {
            step = 0;
        }
    }
}

/** An animated texture: the mat with this frame's image, as the batch stands now. */
static __attribute__((noinline, cold)) const pvr_poly_hdr_t *nat_anim_header(const NatChunk *chunk, s32 segment, s32 mat,
                                                                             pvr_poly_hdr_t *out) {
    const TriangleBatchInfo *batch = &gCurrentLevelModel->segments[segment].batches[chunk->batch];
    s32 t = sInfos[mat].texture;
    const u32 *frames = sFrameWords[t];
    TextureHeader *tex = gCurrentLevelModel->textures[t].texture;
    s32 offset = batch->texOffset << 14;

    if (frames == NULL) {
        return &sMats[mat];
    }
    shz_memcpy32(out, &sMats[mat], sizeof(*out));
    if (offset && offset < (tex->numOfTextures << 8)) {
        out->mode3 = frames[offset >> 16];
    } else {
        out->mode3 = frames[0];
    }
    return out;
}

/**
 * What a combiner does with the shade under this frame's colours. A rule says it
 * without running the combiner; pc_nat_classify is for what the rule leaves open.
 */
static inline s32 nat_shade_by(u32 rule, u32 w0, u32 w1, u32 cycle, const NatView *view, u32 *keep, u32 *konst) {
    if (rule == NAT_CC_SHADE && !(view->env & 0xFF)) {
        *keep = 0x00FFFFFFu;
        *konst = view->prim << 24;
        return TRUE;
    }
    if (rule == NAT_CC_WHITE) {
        *keep = 0;
        *konst = 0x00FFFFFFu | (view->prim << 24);
        return TRUE;
    }
    return pc_nat_classify(w0, w1, cycle, view->prim, view->env, keep, konst);
}

DKR_NAT_CODE void pc_nat_level_run(const NatView *view, const NatRec *rec_) {
    const NatChunk *chunk = &sChunks[rec_->b];
    const NatChunk *end = &sChunks[rec_->c];
    const u32 hidden = (rec_->a & 0x10000) ? NAT_CHUNK_WATER : 0;
    const s32 segment = rec_->a & 0xFFFF;
    const u32 glare = gDcGlareAdd;
    const s32 fade = gDcFadeOn;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    s32 lastMat = -1;

    if (sPlanesFor != view->matrix) {
        nat_planes(view->matrix);
    }
    gfx_nat_scale(&scaleX, &scaleY);
    sx = view->vpScaleX * scaleX;
    sy = view->vpScaleY * scaleY;
    tx = view->vpTransX * scaleX;
    ty = view->vpTransY * scaleY;
    shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);

    for (; chunk < end; chunk++) {
        s32 part;

        if ((chunk->flags & hidden) || nat_outside(chunk)) {
            continue;
        }
        {
            const s32 textured = (chunk->flags & NAT_CHUNK_TEXTURED) != 0;
            const f32 zMul = (chunk->flags & NAT_CHUNK_DECAL) ? 1.003f : 1.0f;
            const u32 offset = textured ? glare : 0;
            s32 behind = nat_transform(chunk->xyz, chunk->numPositions, sx, sy, tx, ty, zMul);
            const NatCorner *corners = chunk->corners;

            for (part = 0; part < 2; part++) {
                s32 count = part ? chunk->twoSided : chunk->oneSided;
                s32 mat = part ? chunk->matTwo : chunk->matOne;

                if (count == 0) {
                    continue;
                }
                if (mat != lastMat || (chunk->flags & NAT_CHUNK_TEX_ANIM)) {
                    pvr_poly_hdr_t animated __attribute__((aligned(32)));
                    const pvr_poly_hdr_t *hdr = &sMats[mat];

                    if (chunk->flags & NAT_CHUNK_TEX_ANIM) {
                        hdr = nat_anim_header(chunk, segment, mat, &animated);
                    }
                    gfx_nat_header(hdr);
                    lastMat = mat;
                }
                if (behind) {
                    nat_clip_level(chunk, corners, count, !part, scaleX, scaleY, zMul, offset, textured);
                } else if (view->exact) {
                    nat_write_exact(corners, count, offset, FALSE, textured, zMul, scaleX, scaleY);
                } else if (fade) {
                    nat_write_faded(corners, count, offset, textured);
                } else {
                    nat_write_sq(corners, count, offset, FALSE);
                }
                corners += count;
            }
        }
    }
}

// An object model's block (dreamcast/build_native_meshes.py), and what the
// console worked out from it.
typedef struct {
    char magic[4];
    u32 fileSize, numMats, numChunks, matsAt, infosAt, chunksAt, cornersAt;
    f32 centre[3], half;
} NatModelHeader;

typedef struct {
    u16 firstVertex, numVertices, oneSided, twoSided, flags, unused;
    union {
        u32 cornersAt;
        const NatCorner *corners;
    };
    u16 mat[4]; // by variant
    u32 unused2[2];
} NatModelChunk;

_Static_assert(sizeof(NatModelChunk) == 32, "NatModelChunk is what the pack holds");

typedef struct {
    NatModelHeader *block;
    pvr_poly_hdr_t *mats;
    const NatMatInfo *infos;
    NatModelChunk *chunks;
    s32 generation; // the video memory its mats are good for
    s32 variants;   // bit per variant: every batch has its mat
    u32 **frameWords; // a texture's frames as header words: per model texture, NULL if it has one frame
    u8 lists[4];    // per variant: a bit for each list its batches go to
} NatModel;

#define NAT_MODEL(model) (*(NatModel **) (model)->pad34)
#define NAT_VARIANT_FADING 1
#define NAT_VARIANT_DIRECTIONAL 2
#define NAT_CHUNK_ENVMAP 8

static void nat_model_frames_free(const ObjectModel *model, NatModel *nat) {
    s32 i;

    if (nat->frameWords != NULL) {
        for (i = 0; i < model->numberOfTextures; i++) {
            free(nat->frameWords[i]);
        }
        free(nat->frameWords);
        nat->frameWords = NULL;
    }
}

void pc_nat_model_free(ObjectModel *model) {
    NatModel *nat = NAT_MODEL(model);

    if (nat != NULL) {
        nat_model_frames_free(model, nat);
        free(nat->block);
        free(nat);
        NAT_MODEL(model) = NULL;
    }
}

/** Reads a model's block from the pack, for a model the game has just loaded. */
void pc_nat_model_load(ObjectModel *model, s32 modelId) {
    u8 *block;
    NatModel *nat;
    s32 i;

    NAT_MODEL(model) = NULL;
    block = nat_pack_read(sPackIndex != NULL || nat_pack_open() ? sPackLevels + modelId : 0xFFFFFFFFu);
    if (block == NULL) {
        return;
    }
    if (__builtin_memcmp(block, "DNMO", 4) != 0 || ((NatModelHeader *) block)->fileSize != (u32) model->fileSize ||
        ((NatModelHeader *) block)->numChunks != (u32) model->numberOfBatches) {
        free(block);
        return;
    }
    nat = malloc(sizeof(*nat));
    if (nat == NULL) {
        printf("native: malloc failed for object model %ld\n", (long) modelId);
        free(block);
        return;
    }
    nat->block = (NatModelHeader *) block;
    nat->mats = (pvr_poly_hdr_t *) (block + nat->block->matsAt);
    nat->infos = (const NatMatInfo *) (block + nat->block->infosAt);
    nat->chunks = (NatModelChunk *) (block + nat->block->chunksAt);
    nat->generation = 0;
    nat->frameWords = NULL;
    for (i = 0; i < model->numberOfBatches; i++) {
        nat->chunks[i].corners = (const NatCorner *) (block + nat->chunks[i].cornersAt);
    }
    NAT_MODEL(model) = nat;
}

/** A texture's frames, for a batch that shows one of them. `mode3` stands in for a frame that is not in video memory. */
static void nat_model_frames(const ObjectModel *model, NatModel *nat, s32 texture, u32 mode3) {
    const TextureHeader *tex = model->textures[texture].texture;
    s32 frames = tex->numOfTextures >> 8;
    s32 f;

    if (nat->frameWords == NULL || nat->frameWords[texture] != NULL || frames < 2) {
        return;
    }
    nat->frameWords[texture] = malloc(frames * sizeof(u32));
    if (nat->frameWords[texture] != NULL) {
        for (f = 0; f < frames; f++) {
            u32 mode2;

            if (!pc_nat_texture_words((const u8 *) tex + f * tex->textureSize, &mode2, &nat->frameWords[texture][f])) {
                nat->frameWords[texture][f] = mode3;
            }
        }
    }
}

/** The textures' words in the mats, and which variants can be drawn. */
static __attribute__((noinline, cold)) void nat_model_resolve(const ObjectModel *model, NatModel *nat) {
    u32 i;
    s32 c, v;

    nat->generation = sVramGeneration;
    nat->variants = 15;
    nat->lists[0] = nat->lists[1] = nat->lists[2] = nat->lists[3] = 0;
    nat_model_frames_free(model, nat);
    nat->frameWords = calloc(model->numberOfTextures, sizeof(*nat->frameWords));
    for (i = 0; i < nat->block->numMats; i++) {
        const NatMatInfo *info = &nat->infos[i];
        u32 mode2, mode3;

        if (info->texture < 0) {
            continue;
        }
        if (!pc_nat_texture_words(model->textures[info->texture].texture, &mode2, &mode3)) {
            nat->variants = 0;
            return;
        }
        nat->mats[i].mode2 = (nat->mats[i].mode2 & ~(PVR_TA_PM2_USIZE_MASK | PVR_TA_PM2_VSIZE_MASK)) | mode2;
        nat->mats[i].mode3 = mode3;
        nat_model_frames(model, nat, info->texture, mode3);
    }
    for (c = 0; c < model->numberOfBatches; c++) {
        for (v = 0; v < 4; v++) {
            if (model->batches[c].flags & RENDER_HIDDEN) {
                continue;
            }
            if (nat->chunks[c].mat[v] == 0xFFFF) {
                nat->variants &= ~(1 << v);
            } else {
                nat->lists[v] |= 1 << nat->infos[nat->chunks[c].mat[v]].route;
            }
        }
    }
}

/** The whole model, at rest, against the view: animation moves its vertices, so the box is taken at twice the size. */
static s32 nat_model_outside(const NatModel *nat, const f32 *matrix) {
    const NatModelHeader *block = nat->block;
    NatChunk box;

    box.centre[0] = block->centre[0];
    box.centre[1] = block->centre[1];
    box.centre[2] = block->centre[2];
    box.half[0] = box.half[1] = box.half[2] = block->half * 2.0f;
    if (sPlanesFor != matrix) {
        nat_planes(matrix);
    }
    return nat_outside(&box);
}

s32 pc_nat_model_draw(ObjectModel *model, const Vertex *verts, s32 start, s32 flags, s32 overrideVerts) {
    extern s32 gBlockedRenderFlags;
    extern s16 gUseDirectionalLighting;
    NatModel *nat = NAT_MODEL(model);
    NatRec *rec;
    s32 variant, i;

    if (nat == NULL || gBlockedRenderFlags != 0) {
        return -2;
    }
    if (nat->generation != sVramGeneration) {
        nat_model_resolve(model, nat);
    }
    variant = ((flags & RENDER_SEMI_TRANSPARENT) ? NAT_VARIANT_FADING : 0) |
              (gUseDirectionalLighting ? NAT_VARIANT_DIRECTIONAL : 0);
    if (!(nat->variants & (1 << variant))) {
        return -2;
    }
    if (!nat_model_outside(nat, gNat.matrix[gNat.cur]) && (rec = pc_nat_record(NAT_REC_MODEL)) != NULL) {
        rec->a = start | (variant << 8) | (overrideVerts ? 0x400 : 0);
        rec->p0 = verts;
        rec->p1 = model;
        pc_nat_depth_drawn();
        // Each list draws the batches that are its own.
        for (i = NAT_LIST_OP; i <= NAT_LIST_TR; i++) {
            if (nat->lists[variant] & (1 << i)) {
                pc_nat_bin(i, rec);
            }
        }
    }
    // As render_mesh's walk over the batches would stop.
    for (i = start; i < model->numberOfBatches; i++) {
        if ((model->batches[i].flags & RENDER_SEMI_TRANSPARENT) && !(flags & RENDER_SEMI_TRANSPARENT)) {
            return i;
        }
    }
    return -1;
}

/**
 * The vertices through the matrix in XMTRX, to sPoints from `out` on. Their
 * positions change every frame, so these are the game's s16s.
 */
static inline s32 nat_transform_vertices(NatPoint *out, const Vertex *v, s32 count, f32 sx, f32 sy, f32 tx, f32 ty,
                                         f32 zMul) {
    const Vertex *end = v + count;
    s32 behind = 0;

    for (; v + 1 < end; v += 2, out += 2) {
        shz_vec4_t c0 = shz_xmtrx_transform_vec4(shz_vec4_init(v[0].x, v[0].y, v[0].z, 1.0f));
        shz_vec4_t c1 = nat_transform_fv4(v[1].x, v[1].y, v[1].z);
        f32 inv0 = shz_invf_fsrra(c0.w);
        f32 inv1 = shz_invf_fsrra(c1.w);

        behind |= (c0.w < 1.0f) | (c1.w < 1.0f);
        out[0].x = (c0.x * inv0 * sx) + tx;
        out[0].y = ty - (c0.y * inv0 * sy);
        out[0].z = inv0 * zMul;
        out[1].x = (c1.x * inv1 * sx) + tx;
        out[1].y = ty - (c1.y * inv1 * sy);
        out[1].z = inv1 * zMul;
    }
    if (v < end) {
        shz_vec4_t c = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
        f32 inv = shz_invf_fsrra(c.w);

        behind |= c.w < 1.0f;
        out->x = (c.x * inv * sx) + tx;
        out->y = ty - (c.y * inv * sy);
        out->z = inv * zMul;
    }
    return behind;
}

/** A model's strips when a position lies behind the near plane: the clip space positions again, for the clipper. */
static __attribute__((noinline, cold)) void nat_clip_model(const NatView *view, const Vertex *v, s32 count, s32 split,
                                                           const NatCorner *corners, s32 numCorners, s32 cull, f32 scaleX,
                                                           f32 scaleY, f32 zMul, u32 offset, s32 textured) {
    shz_vec4_t clip[NAT_MAX_POSITIONS];
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);
    for (i = 0; i < count; i++) {
        if (i == split) {
            shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix2);
        }
        clip[i] = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
        argb[i] = sPoints[i].argb;
    }
    pc_nat_clip(corners, numCorners, clip, argb, cull, scaleX, scaleY, zMul, offset, textured);
}

/** A made-up batch's triangles past the near plane, one by one through the clipper. */
static __attribute__((noinline, cold)) void nat_clip_tris(const NatView *view, const Vertex *v, s32 count, s32 split,
                                                          const Triangle *tris, s32 numTris, const NatMaterial *mat, f32 scaleX,
                                                          f32 scaleY, f32 zMul, u32 offset) {
    shz_vec4_t clip[NAT_MAX_POSITIONS];
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);
    for (i = 0; i < count; i++) {
        if (i == split) {
            shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix2);
        }
        clip[i] = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
        argb[i] = sPoints[i].argb;
    }
    for (i = 0; i < numTris; i++) {
        const Triangle *t = &tris[i];
        NatCorner corners[3];

        if (t->vi0 >= count || t->vi1 >= count || t->vi2 >= count) {
            continue;
        }
        corners[0] = (NatCorner) { t->uv0.u * mat->uMul, t->uv0.v * mat->vMul, 0, PVR_CMD_VERTEX | t->vi0 };
        corners[1] = (NatCorner) { t->uv1.u * mat->uMul, t->uv1.v * mat->vMul, 0, PVR_CMD_VERTEX | t->vi1 };
        corners[2] = (NatCorner) { t->uv2.u * mat->uMul, t->uv2.v * mat->vMul, 0, PVR_CMD_VERTEX_EOL | t->vi2 };
        pc_nat_clip(corners, 3, clip, argb, !(t->flags & BACKFACE_DRAW), scaleX, scaleY, zMul, offset, mat->textured);
    }
}

/** Environment-mapped corners: their coordinates as the object's lighting left them in the model. */
static __attribute__((noinline, cold)) const NatCorner *nat_envmap_corners(const ObjectModel *model, const NatCorner *c,
                                                                           s32 count, NatCorner *out) {
    s32 i;

    for (i = 0; i < count; i++) {
        const Triangle *t = &model->triangles[c[i].argb >> 2];
        const TexCoords *uv = (c[i].argb & 3) == 0 ? &t->uv0 : (c[i].argb & 3) == 1 ? &t->uv1 : &t->uv2;

        out[i].u = uv->u * c[i].u;
        out[i].v = uv->v * c[i].v;
        out[i].argb = 0;
        out[i].word = c[i].word;
    }
    return out;
}

/** A texture's other frame: the mat with its image. */
static __attribute__((noinline, cold)) const pvr_poly_hdr_t *nat_frame_header(const pvr_poly_hdr_t *mat, const NatModel *nat,
                                                                              s32 texture, s32 frame, pvr_poly_hdr_t *out) {
    shz_memcpy32(out, mat, sizeof(*out));
    if (nat->frameWords != NULL && nat->frameWords[texture] != NULL) {
        out->mode3 = nat->frameWords[texture][frame];
    }
    return out;
}

/** The vertices' colours, from a combiner that does not collapse, or with a fade drawn into the frame. */
static __attribute__((noinline, cold)) void nat_colours_slow2(const Vertex *v, s32 at, s32 count) {
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    pc_nat_colours_mux(argb, v, count);
    for (i = 0; i < count; i++) {
        sPoints[at + i].argb = argb[i];
    }
}

static __attribute__((noinline, cold)) void nat_colours_slow(const Vertex *v, s32 count, s32 fast, s32 textured, s32 fade) {
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    if (!fast) {
        pc_nat_colours_mux(argb, v, count);
        for (i = 0; i < count; i++) {
            sPoints[i].argb = argb[i];
        }
    }
    if (fade) {
        for (i = 0; i < count; i++) {
            sPoints[i].argb = gfx_frame_argb(sPoints[i].argb, textured);
        }
    }
}

DKR_NAT_CODE void pc_nat_model_run(const NatView *view, const NatRec *rec_) {
    const ObjectModel *model = (const ObjectModel *) rec_->p1;
    const NatModel *nat = NAT_MODEL(model);
    const Vertex *verts = (const Vertex *) rec_->p0;
    const s32 variant = (rec_->a >> 8) & 3;
    const s32 fading = variant & NAT_VARIANT_FADING;
    const s32 overrideVerts = (rec_->a >> 10) & 1;
    const u32 glare = gDcGlareAdd;
    const s32 fade = gDcFadeOn;
    u32 lastCombiner = 0xFFFFFFFFu;
    s32 i = rec_->a & 0xFF;
    s32 lastMat = -1;
    s32 fast = TRUE;
    u32 keep = 0, konst = 0;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;

    gfx_nat_scale(&scaleX, &scaleY);
    sx = view->vpScaleX * scaleX;
    sy = view->vpScaleY * scaleY;
    tx = view->vpTransX * scaleX;
    ty = view->vpTransY * scaleY;

    for (; i < model->numberOfBatches; i++) {
        const TriangleBatchInfo *batch = &model->batches[i];
        const NatModelChunk *chunk = &nat->chunks[i];
        const NatMatInfo *info;
        const NatCorner *corners = chunk->corners;
        NatCorner env[NAT_MAX_CORNERS];
        const Vertex *v;
        s32 count, split, part, behind, m;
        const u32 flags = batch->flags;
        f32 zMul;
        u32 offset;

        if ((flags & RENDER_SEMI_TRANSPARENT) && !fading) {
            break;
        }
        if (flags & RENDER_HIDDEN) {
            continue;
        }
        info = &nat->infos[chunk->mat[variant]];
        if (info->route != sListRoute) {
            continue;
        }
        count = chunk->numVertices;
        v = &verts[chunk->firstVertex];
        zMul = info->decal ? 1.003f : 1.0f;
        offset = info->texture >= 0 ? glare : 0;
        split = overrideVerts ? batch->vertOverride : count;
        split = split < 0 ? 0 : split > count ? count : split;
        behind = 0;
        if (split > 0) {
            shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);
            behind |= nat_transform_vertices(sPoints, v, split, sx, sy, tx, ty, zMul);
        }
        if (split < count) {
            shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix2);
            behind |= nat_transform_vertices(&sPoints[split], &v[split], count - split, sx, sy, tx, ty, zMul);
        }
        if (info->combiner != lastCombiner) {
            lastCombiner = info->combiner;
            fast = nat_shade_by(info->combiner & NAT_CC_RULE, info->combine0, info->combine1, info->cycle, view, &keep, &konst);
        }
        for (m = 0; m < count; m++) {
            sPoints[m].argb = ((((u32) v[m].a << 24) | ((u32) v[m].r << 16) | ((u32) v[m].g << 8) | (u32) v[m].b) & keep) |
                              konst;
        }
        if (!fast || fade) {
            nat_colours_slow(v, count, fast, info->texture >= 0, fade);
        }
        for (part = 0; part < 2; part++) {
            s32 numCorners = part ? chunk->twoSided : chunk->oneSided;
            s32 mat = chunk->mat[variant] + part;
            s32 frame = 0;

            if (numCorners == 0) {
                continue;
            }
            if (info->texture >= 0 && batch->texOffset != 0) {
                TextureHeader *tex = model->textures[info->texture].texture;
                s32 texOffset = batch->texOffset << 14;

                if (texOffset < (tex->numOfTextures << 8)) {
                    frame = texOffset >> 16;
                }
            }
            if (mat != lastMat || frame != 0) {
                pvr_poly_hdr_t framed __attribute__((aligned(32)));
                const pvr_poly_hdr_t *hdr = &nat->mats[mat];

                if (frame != 0) {
                    hdr = nat_frame_header(hdr, nat, info->texture, frame, &framed);
                }
                gfx_nat_header(hdr);
                lastMat = frame != 0 ? -1 : mat;
            }
            {
                const NatCorner *c = (chunk->flags & NAT_CHUNK_ENVMAP) ? nat_envmap_corners(model, corners, numCorners, env)
                                                                       : corners;

                if (behind) {
                    nat_clip_model(view, v, count, split, c, numCorners, !part, scaleX, scaleY, zMul, offset,
                                   info->texture >= 0);
                } else if (view->exact) {
                    nat_write_exact(c, numCorners, offset, TRUE, info->texture >= 0, zMul, scaleX, scaleY);
                } else {
                    nat_write_sq(c, numCorners, offset, TRUE);
                }
            }
            corners += numCorners;
        }
    }
}

// Materials that are not known offline: a particle's, a shadow's, the water's.
// One is compiled the first time the game asks for it, as its scene starts drawing,
// and kept until the textures change.
#define NAT_MATERIALS 512

static NatMaterial sMaterials[NAT_MATERIALS];

// A sprite's tile comes back every frame with the same table entry and texture.
#define NAT_TILES 256
static struct {
    const void *entry, *tex;
    const NatMaterial *mat;
    u32 geometry;
} sTiles[NAT_TILES] __attribute__((aligned(32)));

void pc_nat_materials_forget(void) {
    s32 i;

    for (i = 0; i < NAT_MATERIALS; i++) {
        sMaterials[i].used = FALSE;
    }
    shz_memset8(sTiles, 0, sizeof(sTiles));
}

/**
 * A combiner's rule, for a material made up on the console: the same trial as
 * dreamcast/build_native_meshes.py combiner_rule, under a few colours.
 */
static u32 nat_material_rule(u32 w0, u32 w1, u32 cycle) {
    static const u32 sPrims[3] = { 0xFFFFFFFFu, 0x0D5A0025u, 0x00FF03FEu };
    u32 shade = TRUE, white = TRUE;
    s32 i;

    for (i = 0; i < 3; i++) {
        u32 keep, konst;

        shade &= pc_nat_classify(w0, w1, cycle, sPrims[i], 0x09820700u, &keep, &konst) && keep == 0x00FFFFFFu &&
                 konst == sPrims[i] << 24;
        white &= pc_nat_classify(w0, w1, cycle, sPrims[i], 0x01020304u, &keep, &konst) && keep == 0 &&
                 konst == (0x00FFFFFFu | (sPrims[i] << 24));
    }
    return shade ? NAT_CC_SHADE : white ? NAT_CC_WHITE : 0;
}

/** The list these modes draw to. */
static s32 nat_route(u32 omh, u32 oml, u32 geometry) {
    s32 twoCycle = (omh & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    u32 p = (oml >> (twoCycle ? 28 : 30)) & 3;
    u32 q = (oml >> (twoCycle ? 20 : 22)) & 3;

    if ((oml & CVG_X_ALPHA) || ((oml & 3) == G_AC_THRESHOLD && ((geometry >> 2) & 1))) {
        return NAT_LIST_PT;
    }
    return (oml & FORCE_BL) && (p == G_BL_CLR_MEM || q == G_BL_CLR_MEM) ? NAT_LIST_TR : NAT_LIST_OP;
}

/** As the walk's render-mode code and gfx.c's header compile would come out for these modes. */
static __attribute__((noinline, cold)) const NatMaterial *nat_material_compile(NatMaterial *m, u32 w0, u32 w1, u32 omh, u32 oml,
                                                                               const NatTexture *tex, u32 geometry) {
    pvr_poly_cxt_t cxt;
    u32 mode2 = 0, mode3 = 0;
    s32 zBuffer = geometry & 1, fog = (geometry >> 1) & 1, thresholdOk = (geometry >> 2) & 1;
    s32 depthTest = zBuffer && (oml & Z_CMP);
    s32 depthWrite = zBuffer && (oml & Z_UPD);
    s32 twoCycle = (omh & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    u32 p = (oml >> (twoCycle ? 28 : 30)) & 3;
    u32 q = (oml >> (twoCycle ? 20 : 22)) & 3;
    s32 blend = (oml & FORCE_BL) && (p == G_BL_CLR_MEM || q == G_BL_CLR_MEM);
    s32 alphaTest = (oml & CVG_X_ALPHA) || ((oml & 3) == G_AC_THRESHOLD && thresholdOk);
    s32 textured = tex != NULL && tex->handle != 0;
    s32 route = nat_route(omh, oml, geometry);

    m->depth = depthTest;
    m->depthWrite = depthWrite;
    m->blend = blend;
    m->alphaTest = alphaTest;
    m->fog = fog;
    m->point = ((omh >> G_MDSFT_TEXTFILT) & 3) == 0;
    if (textured) {
        gfx_texture_set_wrap(tex->handle, tex->cms, tex->cmt);
        if (!gfx_texture_words(tex->handle, (unsigned int *) &mode2, (unsigned int *) &mode3)) {
            textured = FALSE;
        }
    }
    pvr_poly_cxt_col(&cxt, route == 0 ? PVR_LIST_OP_POLY : route == 1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY);
    if (textured) {
        cxt.txr.enable = TRUE;
        cxt.txr.filter = m->point ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        cxt.txr.width = 8;
        cxt.txr.height = 8;
        cxt.txr.format = 0;
        cxt.txr.base = NULL;
        cxt.txr.mipmap = PVR_MIPMAP_DISABLE;
        cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        cxt.txr.uv_flip = ((tex->cms & 1) ? PVR_UVFLIP_U : 0) | ((tex->cmt & 1) ? PVR_UVFLIP_V : 0);
        cxt.txr.uv_clamp = (tex->cms & 2) ? ((tex->cmt & 2) ? PVR_UVCLAMP_UV : PVR_UVCLAMP_U)
                                          : ((tex->cmt & 2) ? PVR_UVCLAMP_V : PVR_UVCLAMP_NONE);
    }
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.clip_mode = PVR_USERCLIP_DISABLE;
    cxt.gen.specular = PVR_SPECULAR_ENABLE;
    cxt.gen.fog_type = fog ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = depthWrite ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    if (route == 0) {
        cxt.gen.alpha = PVR_ALPHA_DISABLE;
        cxt.txr.alpha = PVR_TXRALPHA_DISABLE;
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
    } else {
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    }
    pvr_poly_compile(&m->hdr, &cxt);
    m->hdr.mode2 |= mode2;
    m->hdr.mode3 = mode3;
    m->uMul = m->vMul = 0.0f;
    if (textured) {
        f32 uScale, vScale;

        gfx_texture_scale(tex->handle, &uScale, &vScale);
        m->uMul = uScale / (32.0f * tex->width);
        m->vMul = vScale / (32.0f * tex->height);
    }
    m->combine0 = w0;
    m->combine1 = w1;
    m->cycle = omh & (3 << G_MDSFT_CYCLETYPE);
    m->rule = nat_material_rule(w0, w1, m->cycle);
    m->otherH = omh;
    m->otherL = oml;
    m->handle = textured ? tex->handle : 0;
    m->width = textured ? tex->width : 0;
    m->height = textured ? tex->height : 0;
    m->cms = textured ? tex->cms : 0;
    m->cmt = textured ? tex->cmt : 0;
    m->route = route;
    m->decal = zBuffer && (oml & ZMODE_DEC) == ZMODE_DEC;
    m->textured = textured;
    m->geometry = geometry;
    m->used = TRUE;
    return m;
}

/** The material for these modes, compiled if it is new. */
static const NatMaterial *nat_material(u32 w0, u32 w1, u32 omh, u32 oml, const NatTexture *tex, u32 geometry) {
    u32 handle = tex != NULL ? tex->handle : 0;
    u32 hash = (w0 * 0x9E3779B1u) ^ (w1 * 0x85EBCA6Bu) ^ (omh * 0xC2B2AE35u) ^ (oml * 0x27D4EB2Fu) ^
               (handle * 0x165667B1u) ^ geometry;
    u32 at = (hash ^ (hash >> 15) ^ (hash >> 7)) & (NAT_MATERIALS - 1);
    s32 probe;

    for (probe = 0; probe < NAT_MATERIALS; probe++) {
        NatMaterial *m = &sMaterials[(at + probe) & (NAT_MATERIALS - 1)];

        if (!m->used) {
            return nat_material_compile(m, w0, w1, omh, oml, tex, geometry);
        }
        if (m->combine0 == w0 && m->combine1 == w1 && m->otherH == omh && m->otherL == oml && m->handle == handle &&
            m->geometry == geometry) {
            return m;
        }
    }
    printf("native: material table full\n");
    pc_nat_materials_forget();
    return nat_material_compile(&sMaterials[at], w0, w1, omh, oml, tex, geometry);
}

/** The material of what the game has set now. */
const NatMaterial *pc_nat_material_now(s32 textured) {
    u32 geometry = ((gNat.geometry & G_ZBUFFER) ? 1 : 0) | ((gNat.geometry & G_FOG) ? 2 : 0) | ((gNat.blend & 0xFF) ? 4 : 0);

    return nat_material(gNat.combine0, gNat.combine1, gNat.otherH, gNat.otherL, textured ? &gNat.texture : NULL, geometry);
}

u32 pc_nat_material_handle(const NatMaterial *mat) {
    return mat->handle;
}

void pc_nat_material_size(const NatMaterial *mat, s32 *w, s32 *h, s32 *cms, s32 *cmt) {
    *w = mat->width;
    *h = mat->height;
    *cms = mat->cms;
    *cmt = mat->cmt;
}

void pc_nat_material_flags(const NatMaterial *mat, s32 *depth, s32 *write, s32 *decal, s32 *blend, s32 *alpha, s32 *fog, s32 *point) {
    *depth = mat->depth;
    *write = mat->depthWrite;
    *decal = mat->decal;
    *blend = mat->blend;
    *alpha = mat->alphaTest;
    *fog = mat->fog;
    *point = mat->point;
}

/** The modes as the rest of the frame's code reads them. */
void pc_nat_load_state(const NatMaterial *mat, u32 prim, u32 env) {
    pc_nat_set_modes(mat->combine0, mat->combine1, mat->otherH, mat->otherL, prim, env);
}

/**
 * A batch that goes without a depth test, the backdrop and the 2D layer: where it
 * goes depends on what the frame has drawn, which gfx.c keeps track of, and so
 * it takes gfx.c's way, whatever the batch.
 */
static __attribute__((noinline, cold)) void nat_tris_layered(const NatMaterial *mat, const Triangle *tris, s32 numTris, s32 numVerts,
                                                              f32 scaleX, f32 scaleY) {
    static GfxTriVert sVerts[3 * 64] __attribute__((aligned(32)));
    const f32 invU = mat->uMul * mat->width > 0.0f ? 1.0f / (32.0f * mat->width) : 0.0f;
    const f32 invV = mat->vMul * mat->height > 0.0f ? 1.0f / (32.0f * mat->height) : 0.0f;
    s32 count = 0, i, k;

    for (i = 0; i < numTris && count + 3 <= 3 * 64; i++) {
        const Triangle *t = &tris[i];
        const s32 at[3] = { t->vi0, t->vi1, t->vi2 };
        const s32 u[3] = { t->uv0.u, t->uv1.u, t->uv2.u };
        const s32 v[3] = { t->uv0.v, t->uv1.v, t->uv2.v };

        if (at[0] >= numVerts || at[1] >= numVerts || at[2] >= numVerts) {
            continue;
        }
        if (!(t->flags & BACKFACE_DRAW)) {
            const NatPoint *p0 = &sPoints[at[0]], *p1 = &sPoints[at[1]], *p2 = &sPoints[at[2]];

            if (((p1->x - p0->x) * (p2->y - p0->y)) - ((p2->x - p0->x) * (p1->y - p0->y)) >= 0.0f) {
                continue;
            }
        }
        for (k = 0; k < 3; k++, count++) {
            const NatPoint *p = &sPoints[at[k]];
            GfxTriVert *o = &sVerts[count];

            o->x = p->x / scaleX;
            o->y = p->y / scaleY;
            o->z = 0.0f;
            o->w = 1.0f / p->z;
            o->u = u[k] * invU;
            o->v = v[k] * invV;
            o->fog = 0.0f;
            o->a = p->argb >> 24;
            o->r = p->argb >> 16;
            o->g = p->argb >> 8;
            o->b = p->argb;
        }
    }
    pc_nat_draw_tris(mat, sVerts, count);
}

/**
 * One batch of what a frame makes up. Triangles do not share corners here (their
 * coordinates differ and the batch is short), and are culled on the CPU, so no
 * strip to break.
 */
__attribute__((noinline)) void pc_nat_tris_run(const NatView *view, const NatRec *rec_) {
    const NatMaterial *mat = rec_->mat;
    const Vertex *v = (const Vertex *) rec_->p0;
    const Vertex *v2 = (const Vertex *) rec_->p1;
    const Triangle *tris = (const Triangle *) rec_->p2;
    const s32 numLoad = rec_->a;
    const s32 numLoad2 = rec_->b;
    const s32 numVerts = numLoad + numLoad2;
    const s32 numTris = rec_->c;
    const u32 offset = mat->textured ? gDcGlareAdd : 0;
    const f32 uMul = mat->uMul, vMul = mat->vMul;
    const f32 zMul = mat->decal ? 1.003f : 1.0f;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    u32 keep, konst;
    s32 fast, behind, i;
    u32 sq;

    if (numVerts > NAT_MAX_POSITIONS || numVerts == 0) {
        return;
    }
    gfx_nat_scale(&scaleX, &scaleY);
    sx = view->vpScaleX * scaleX;
    sy = view->vpScaleY * scaleY;
    tx = view->vpTransX * scaleX;
    ty = view->vpTransY * scaleY;
    shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);
    behind = nat_transform_vertices(sPoints, v, numLoad, sx, sy, tx, ty, zMul);
    if (numLoad2) {
        shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix2);
        behind |= nat_transform_vertices(&sPoints[numLoad], v2, numLoad2, sx, sy, tx, ty, zMul);
    }
    fast = nat_shade_by(mat->rule, mat->combine0, mat->combine1, mat->cycle, view, &keep, &konst);
    for (i = 0; i < numLoad; i++) {
        sPoints[i].argb = ((((u32) v[i].a << 24) | ((u32) v[i].r << 16) | ((u32) v[i].g << 8) | (u32) v[i].b) & keep) | konst;
    }
    for (i = 0; i < numLoad2; i++) {
        sPoints[numLoad + i].argb =
            ((((u32) v2[i].a << 24) | ((u32) v2[i].r << 16) | ((u32) v2[i].g << 8) | (u32) v2[i].b) & keep) | konst;
    }
    if (!fast) {
        nat_colours_slow(v, numLoad, FALSE, mat->textured, FALSE);
        if (numLoad2) {
            nat_colours_slow2(v2, numLoad, numLoad2);
        }
    }
    // Under a depth test the fade goes in here; gfx.c does it for the layered ones.
    if (gDcFadeOn && mat->depth) {
        nat_colours_slow(v, numVerts, TRUE, mat->textured, TRUE);
    }
    if (!mat->depth) {
        nat_tris_layered(mat, tris, numTris, numVerts, scaleX, scaleY);
        return;
    }
    gfx_nat_header(&mat->hdr);
    if (behind) {
        nat_clip_tris(view, v, numVerts, numLoad, tris, numTris, mat, scaleX, scaleY, zMul, offset);
        return;
    }
    if (view->exact) {
        for (i = 0; i < numTris; i++) {
            const Triangle *t = &tris[i];
            NatCorner c[3];

            if (t->vi0 >= numVerts || t->vi1 >= numVerts || t->vi2 >= numVerts) {
                continue;
            }
            if (!(t->flags & BACKFACE_DRAW)) {
                const NatPoint *p0 = &sPoints[t->vi0], *p1 = &sPoints[t->vi1], *p2 = &sPoints[t->vi2];

                if (((p1->x - p0->x) * (p2->y - p0->y)) - ((p2->x - p0->x) * (p1->y - p0->y)) >= 0.0f) {
                    continue;
                }
            }
            c[0] = (NatCorner) { t->uv0.u * uMul, t->uv0.v * vMul, 0, PVR_CMD_VERTEX | t->vi0 };
            c[1] = (NatCorner) { t->uv1.u * uMul, t->uv1.v * vMul, 0, PVR_CMD_VERTEX | t->vi1 };
            c[2] = (NatCorner) { t->uv2.u * uMul, t->uv2.v * vMul, 0, PVR_CMD_VERTEX_EOL | t->vi2 };
            nat_write_exact(c, 3, offset, TRUE, mat->textured, zMul, scaleX, scaleY);
        }
        return;
    }
    sq = pvr_dr_addr;
    for (i = 0; i < numTris; i++) {
        const Triangle *t = &tris[i];
        const NatPoint *p0, *p1, *p2;
        pvr_vertex_t *o;

        if (t->vi0 >= numVerts || t->vi1 >= numVerts || t->vi2 >= numVerts) {
            continue;
        }
        p0 = &sPoints[t->vi0];
        p1 = &sPoints[t->vi1];
        p2 = &sPoints[t->vi2];
        if (!(t->flags & BACKFACE_DRAW)) {
            f32 area = ((p1->x - p0->x) * (p2->y - p0->y)) - ((p2->x - p0->x) * (p1->y - p0->y));

            if (area >= 0.0f) {
                continue;
            }
        }
        o = (pvr_vertex_t *) (sq ^ 32);
        nat_vertex_put(o, PVR_CMD_VERTEX, p0->x, p0->y, p0->z, t->uv0.u * uMul, t->uv0.v * vMul, p0->argb, offset);
        nat_sq_flush(o);
        o = (pvr_vertex_t *) sq;
        nat_vertex_put(o, PVR_CMD_VERTEX, p1->x, p1->y, p1->z, t->uv1.u * uMul, t->uv1.v * vMul, p1->argb, offset);
        nat_sq_flush(o);
        o = (pvr_vertex_t *) (sq ^ 32);
        nat_vertex_put(o, PVR_CMD_VERTEX_EOL, p2->x, p2->y, p2->z, t->uv2.u * uMul, t->uv2.v * vMul, p2->argb, offset);
        nat_sq_flush(o);
        sq ^= 32;
    }
    pvr_dr_addr = sq;
}

/**
 * What a batch without a depth test emits: the first vertex's alpha, as the frame
 * will work it out. If the game fills the vertices in after the draw this reads
 * last time's; all that hangs on it is whether an opaque batch may open the frame.
 */
static s32 nat_tris_alpha(const NatRec *rec) {
    const NatMaterial *mat = rec->mat;
    const Vertex *v = (const Vertex *) rec->p0;
    const Vertex *v2 = (const Vertex *) rec->p1;
    const Triangle *tris = (const Triangle *) rec->p2;
    s32 at = tris[0].vi0;
    u32 keep, konst, argb;

    if (at >= (s32) rec->a) {
        v = v2;
        at -= rec->a;
    }
    if (pc_nat_classify(mat->combine0, mat->combine1, mat->cycle, rec->prim, rec->env, &keep, &konst)) {
        return (((u32) v[at].a << 24 & keep) | konst) >> 24;
    }
    pc_nat_colours_mux(&argb, &v[at], 1);
    return argb >> 24;
}

void pc_nat_bin_tris(NatRec *rec) {
    const NatMaterial *mat = rec->mat;
    const s32 backdrop = pc_nat_view_backdrop();

    if (mat->depth) {
        pc_nat_depth_drawn();
        pc_nat_bin(mat->route, rec);
    } else if (backdrop) {
        pc_nat_bin_2d(rec, mat->alphaTest, 0xFF, mat->handle);
    } else {
        pc_nat_bin_2d(rec, mat->alphaTest, pc_nat_may_open() ? nat_tris_alpha(rec) : 0, mat->handle);
    }
}

/** A sprite's material for one of its tiles. */
static const NatMaterial *nat_sprite_material(const Gfx *entry, u32 geometry, const TextureHeader *tex) {
    u32 at = (((u32) tex >> 5) ^ ((u32) tex >> 13) ^ ((u32) entry >> 3) ^ geometry) & (NAT_TILES - 1);
    const NatMaterial *mat;
    NatTexture t;

    if (sTiles[at].tex == tex && sTiles[at].entry == entry && sTiles[at].geometry == geometry) {
        return sTiles[at].mat;
    }
    pc_nat_texture_of(tex, &t);
    mat = nat_material(entry[0].words.w0, entry[0].words.w1, entry[1].words.w0 & 0x00FFFFFF, entry[1].words.w1, &t, geometry);
    // Not one whose texture is yet to come.
    if (mat != NULL && mat->handle != 0) {
        sTiles[at].tex = tex;
        sTiles[at].entry = entry;
        sTiles[at].geometry = geometry;
        sTiles[at].mat = mat;
    }
    return mat;
}

/**
 * A 2D sprite that may open the frame. It goes as one: to the opaque list only if
 * every tile of it has no transparent texel; its alpha is its first tile's.
 */
static __attribute__((noinline, cold)) void nat_sprite_bin_open(NatRec *rec, const Sprite *sprite, s32 frame, const Gfx *entry,
                                                                u32 geometry) {
    const u16 *tiles = (const u16 *) &sprite->textures[sprite->numberOfTextures];
    const NatMaterial *mat = NULL;
    u32 handle = 0, keep, konst, argb;
    s32 i;

    for (i = tiles[frame]; i < tiles[frame + 1]; i++) {
        const NatMaterial *tile = nat_sprite_material(entry, geometry, sprite->textures[i]);

        if (tile != NULL) {
            mat = mat != NULL ? mat : tile;
            if (handle == 0 || !gfx_texture_opaque(tile->handle)) {
                handle = tile->handle;
            }
            if (!gfx_texture_opaque(handle)) {
                break;
            }
        }
    }
    if (mat == NULL) {
        pc_nat_bin_2d(rec, FALSE, 0, handle);
        return;
    }
    if (pc_nat_classify(mat->combine0, mat->combine1, mat->cycle, rec->prim, rec->env, &keep, &konst)) {
        argb = keep | konst;
    } else {
        pc_nat_colours_mux(&argb, (const Vertex *) sprite->textures - (sprite->numberOfTextures * 4) + (tiles[frame] * 4), 1);
    }
    pc_nat_bin_2d(rec, FALSE, argb >> 24, handle);
}

s32 pc_nat_sprite(const Sprite *sprite, s32 frame, s32 flags, const Mtx *billboard) {
    extern const Gfx *material_simple_lookup(s32 flags, s32 *zBuffer);
    const Gfx *entry;
    NatRec *rec;
    s32 zBuffer;

    if (frame < 0 || frame >= sprite->numberOfFrames) {
        return FALSE;
    }
    entry = material_simple_lookup(flags, &zBuffer);
    rec = pc_nat_record(NAT_REC_SPRITE);
    if (rec != NULL) {
        u32 geometry = (zBuffer ? 1 : 0) | 2 | ((gNat.blend & 0xFF) ? 4 : 0);
        u32 omh = entry[1].words.w0 & 0x00FFFFFF, oml = entry[1].words.w1;
        s32 route = nat_route(omh, oml, geometry);

        rec->a = frame | (geometry << 16);
        rec->p0 = sprite;
        rec->p1 = gNat.billboard ? gNat.verts : NULL;
        rec->p2 = entry;
        rec->matrix = gNat.billboard ? gNat.matrixV : NULL;
        rec->matrix2 = (const f32 *) billboard;
        if (zBuffer && (oml & Z_CMP)) {
            pc_nat_depth_drawn();
            pc_nat_bin(route, rec);
        } else if (route == NAT_LIST_PT || !pc_nat_may_open()) {
            pc_nat_bin_2d(rec, route == NAT_LIST_PT, 0, 0);
        } else {
            nat_sprite_bin_open(rec, sprite, frame, entry, geometry);
        }
    }
    return TRUE;
}


/** A sprite's corner past the near plane: through the clipper, the quad as two triangles. */
static __attribute__((noinline, cold)) void nat_clip_quad(const shz_vec4_t *clip, const u32 *argb, const NatCorner *c, f32 scaleX,
                                                           f32 scaleY, f32 zMul, u32 offset) {
    pc_nat_clip(c, 4, clip, argb, FALSE, scaleX, scaleY, zMul, offset, TRUE);
}

/**
 * A billboard: an anchor in the world, and tiles of four corners each offset from
 * it in clip space through the billboard's own matrix. A vehicle part has no
 * anchor, its corners going through that matrix alone.
 */
__attribute__((noinline)) void pc_nat_sprite_run(const NatView *view, const NatRec *rec_) {
    const Sprite *sprite = (const Sprite *) rec_->p0;
    const Vertex *anchor = (const Vertex *) rec_->p1;
    const f32 *billboard = view->matrix2;
    const Gfx *entry = (const Gfx *) rec_->p2;
    const s32 frame = rec_->a & 0xFFFF;
    const u32 geometry = (rec_->a >> 16) & 7;
    const u16 *tiles = (const u16 *) &sprite->textures[sprite->numberOfTextures];
    const Vertex *verts = (const Vertex *) sprite->textures - (sprite->numberOfTextures * 4);
    shz_vec4_t base = shz_vec4_init(0.0f, 0.0f, 0.0f, 0.0f);
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    s32 i, lastCombine = FALSE;
    u32 keep = 0, konst = 0, combine0 = 0, combine1 = 0, cycle = 0;
    s32 fast = TRUE;

    gfx_nat_scale(&scaleX, &scaleY);
    sx = view->vpScaleX * scaleX;
    sy = view->vpScaleY * scaleY;
    tx = view->vpTransX * scaleX;
    ty = view->vpTransY * scaleY;
    if (anchor != NULL) {
        shz_xmtrx_load_4x4((const shz_mat4x4_t *) view->matrix);
        base = shz_xmtrx_transform_vec4(shz_vec4_init(anchor->x, anchor->y, anchor->z, 1.0f));
    }
    shz_xmtrx_load_4x4((const shz_mat4x4_t *) billboard);
    for (i = tiles[frame]; i < tiles[frame + 1]; i++) {
        const TextureHeader *tex = sprite->textures[i];
        const NatMaterial *mat = nat_sprite_material(entry, geometry, tex);
        const Vertex *v = &verts[i * 4];
        shz_vec4_t clip[4];
        NatCorner c[4];
        u32 argb[4];
        const u32 offset = gDcGlareAdd;
        f32 zMul;
        s32 behind = 0, k;
        f32 uMax, vMax, uMin;

        if (mat == NULL) {
            continue;
        }
        zMul = mat->decal ? 1.003f : 1.0f;
        // !fast: the mux reads what classifying this one left, and a new tile's material may have run over it.
        if (!lastCombine || !fast || mat->combine0 != combine0 || mat->combine1 != combine1 || mat->cycle != cycle) {
            fast = nat_shade_by(mat->rule, mat->combine0, mat->combine1, mat->cycle, view, &keep, &konst);
            combine0 = mat->combine0;
            combine1 = mat->combine1;
            cycle = mat->cycle;
            lastCombine = TRUE;
        }
        for (k = 0; k < 4; k++) {
            clip[k] = shz_vec4_add(shz_xmtrx_transform_vec4(shz_vec4_init(v[k].x, v[k].y, v[k].z, 1.0f)), base);
            behind |= clip[k].w < 1.0f;
        }
        for (k = 0; k < 4; k++) {
            sPoints[k].argb = (0xFFFFFFFFu & keep) | konst;
        }
        if (!fast || (gDcFadeOn && mat->depth)) {
            nat_colours_slow(v, 4, fast, TRUE, gDcFadeOn && mat->depth);
        }
        for (k = 0; k < 4; k++) {
            argb[k] = sPoints[k].argb;
        }
        if (!mat->depth) {
            // The HUD's and the menus' sprites: as sprite_init_frame's two triangles.
            s32 w = (tex->width - 1) << 5, h = (tex->height - 1) << 5;
            Triangle quad[2];

            for (k = 0; k < 4; k++) {
                f32 inv = shz_invf_fsrra(clip[k].w);

                sPoints[k].x = (clip[k].x * inv * sx) + tx;
                sPoints[k].y = ty - (clip[k].y * inv * sy);
                sPoints[k].z = inv;
            }
            quad[0].flags = quad[1].flags = BACKFACE_DRAW;
            quad[0].vi0 = 2;
            quad[0].vi1 = 1;
            quad[0].vi2 = 0;
            quad[0].uv0.u = w;
            quad[0].uv0.v = h;
            quad[0].uv1.u = w;
            quad[0].uv1.v = 0;
            quad[0].uv2.u = 1;
            quad[0].uv2.v = 0;
            quad[1].vi0 = 3;
            quad[1].vi1 = 2;
            quad[1].vi2 = 0;
            quad[1].uv0.u = 1;
            quad[1].uv0.v = h;
            quad[1].uv1.u = w;
            quad[1].uv1.v = h;
            quad[1].uv2.u = 1;
            quad[1].uv2.v = 0;
            nat_tris_layered(mat, quad, 2, 4, scaleX, scaleY);
            continue;
        }
        uMax = ((tex->width - 1) << 5) * mat->uMul;
        vMax = ((tex->height - 1) << 5) * mat->vMul;
        uMin = mat->uMul; // S10.5: the tile's own one, not quite its edge
        // As sprite_init_frame's two triangles, around the shared diagonal.
        c[0] = (NatCorner) { uMax, 0.0f, 0, PVR_CMD_VERTEX | 1 };
        c[1] = (NatCorner) { uMax, vMax, 0, PVR_CMD_VERTEX | 2 };
        c[2] = (NatCorner) { uMin, 0.0f, 0, PVR_CMD_VERTEX | 0 };
        c[3] = (NatCorner) { uMin, vMax, 0, PVR_CMD_VERTEX_EOL | 3 };
        gfx_nat_header(&mat->hdr);
        if (behind) {
            nat_clip_quad(clip, argb, c, scaleX, scaleY, zMul, offset);
        } else if (view->exact) {
            for (k = 0; k < 4; k++) {
                f32 inv = shz_invf_fsrra(clip[k].w);

                sPoints[k].x = (clip[k].x * inv * sx) + tx;
                sPoints[k].y = ty - (clip[k].y * inv * sy);
                sPoints[k].z = inv * zMul;
            }
            nat_write_exact(c, 4, offset, TRUE, TRUE, zMul, scaleX, scaleY);
        } else {
            u32 sq = pvr_dr_addr;

            for (k = 0; k < 4; k++) {
                const shz_vec4_t *p = &clip[c[k].word & 3];
                f32 inv = shz_invf_fsrra(p->w);
                pvr_vertex_t *vt;

                sq ^= 32;
                vt = (pvr_vertex_t *) sq;
                nat_vertex_put(vt, c[k].word & 0xF0000000u, (p->x * inv * sx) + tx, ty - (p->y * inv * sy), inv * zMul, c[k].u,
                               c[k].v, argb[c[k].word & 3], offset);
                nat_sq_flush(vt);
            }
            pvr_dr_addr = sq;
        }
    }
}

// A list's pass: what the viewport and the scissor have come to, as far as it got.
static NatView sView;

void pc_nat_list_begin(pvr_list_t list) {
    sListRoute = list == PVR_LIST_OP_POLY ? NAT_LIST_OP : list == PVR_LIST_PT_POLY ? NAT_LIST_PT : NAT_LIST_TR;
    pc_nat_run_viewport(NULL, &sView);
}

/** One record of a list, enDjinn calling, with what it needs. */
void pc_nat_item(void *data) {
    const NatRec *rec = data;

#if DKR_PROF_SKIP
    if (gfx_prof_list_skipped()) {
        return;
    }
#endif
    sView.matrix = rec->matrix;
    sView.matrix2 = rec->matrix2;
    sView.prim = rec->prim;
    sView.env = rec->env;
    switch (rec->kind) {
        case NAT_REC_LEVEL:
            gfx_nat_set_fog(rec->fog);
            sView.exact = gfx_nat_exact();
            pc_nat_level_run(&sView, rec);
            break;
        case NAT_REC_MODEL:
            gfx_nat_set_fog(rec->fog);
            sView.exact = gfx_nat_exact();
            pc_nat_model_run(&sView, rec);
            break;
        case NAT_REC_SPRITE:
            gfx_nat_set_fog(rec->fog);
            sView.exact = gfx_nat_exact();
            gfx_set_place(rec->place, rec->layer);
            pc_nat_sprite_run(&sView, rec);
            break;
        case NAT_REC_TRIS:
            gfx_nat_set_fog(TRUE);
            sView.exact = gfx_nat_exact();
            gfx_set_place(rec->place, rec->layer);
            pc_nat_tris_run(&sView, rec);
            break;
        case NAT_REC_RECT:
            gfx_set_place(rec->place, rec->layer);
            pc_nat_run_rects(rec);
            break;
        case NAT_REC_FILL:
            gfx_set_place(rec->place, rec->layer);
            pc_nat_run_fill(rec);
            break;
        case NAT_REC_VIEWPORT:
            pc_nat_run_viewport(rec->p0, &sView);
            break;
        case NAT_REC_SCISSOR:
            pc_nat_run_scissor(rec->f);
            break;
        case NAT_REC_FOG_COLOUR:
            pc_nat_run_fog_colour(rec->b);
            break;
        case NAT_REC_FOG_DEPTH:
            pc_nat_run_fog_depth(rec->b);
            break;
        case NAT_REC_TAG:
            pc_nat_run_tag(rec->b, rec->matrix);
            break;
    }
}
