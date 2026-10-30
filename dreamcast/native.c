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
#include <kos.h>
#include "../src/textures_sprites.h"
#include "native.h"

#define NAT_MAX_POSITIONS 32
#define NAT_MAX_CORNERS 64 // sixteen triangles at most in a batch
#define NAT_CHUNK_WATER 1
#define NAT_CHUNK_TEX_ANIM 2
#define NAT_BATCH_WALK 0xFFFF
#define NAT_BATCH_NONE 0xFFFE

#ifndef DKR_NATIVE_CHECK
#define DKR_NATIVE_CHECK 0
#endif
#ifndef DKR_NATIVE_SKIP
#define DKR_NATIVE_SKIP 0
#endif

struct NatMaterial {
    pvr_poly_hdr_t hdr; // no culling: these go out as triangles, culled here
    f32 uMul, vMul;
    u32 combine0, combine1, cycle;
    const void *texture;
    u32 otherL, otherH;
    u8 route, decal, textured, geometry;
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
    u16 frame, unused;
    u32 combine0, combine1, cycle; // what the walk's combiner would be set to
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
static DKR_HOT s32 sPlanesFor = -1;

// A position as a frame leaves it, and for an object model its colour.
typedef struct {
    f32 x, y, z;
    u32 argb;
} NatPoint;

static DKR_HOT NatPoint sPoints[NAT_MAX_POSITIONS] __attribute__((aligned(32)));

extern LevelModel *gCurrentLevelModel;

#if DKR_NATIVE_CHECK
static u32 sChecked, sCheckBad;

/** One line per kind of difference, or a batch repeated every frame would use them all up. */
static s32 nat_check_report(u32 walk2, u32 ours2, s32 route) {
    static u32 sSeen[64];
    static s32 sSeenCount;
    u32 key = (walk2 * 33) ^ ours2 ^ ((u32) route << 28);
    s32 i;

    sCheckBad++;
    for (i = 0; i < sSeenCount; i++) {
        if (sSeen[i] == key) {
            return FALSE;
        }
    }
    if (sSeenCount == 64) {
        return FALSE;
    }
    sSeen[sSeenCount++] = key;
    return TRUE;
}
#endif

void pc_nat_level_free(void) {
#if DKR_NATIVE_CHECK
    printf("NATCHK level done: %lu batches compared, %lu differ\n", (unsigned long) sChecked, (unsigned long) sCheckBad);
#endif
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
    if (fs_read(sPack, head, 16) != 16 || head[0] != 0x4B504E44 /* DNPK */ || head[1] != 2) {
        printf("native: mesh pack is not version 2\n");
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

s32 pc_nat_level_batches(Gfx **dl, s32 segment, s32 batch, s32 end, s32 waterHidden) {
    extern s32 cam_get_viewport_layout(void);
    const NatSegment *seg;
    const u16 *table;
    s32 next;
    s32 first = -1, last = -1;

    if (sLevel == NULL || (DKR_NATIVE_SKIP & 1)) {
        return 0;
    }
    if (!sResolved) {
        nat_level_resolve();
    }
    // Split-screen scissors are not on the PVR's tiles; the walk clips by hand.
    if (cam_get_viewport_layout() != 0) {
        return 0;
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
            if (!sMatGood[sChunks[chunk].matOne]) {
                if (next == batch) {
                    return 0;
                }
                break;
            }
            if (first < 0) {
                first = chunk;
            }
            last = chunk;
        }
    }
    if (first >= 0) {
        gDkrNative((*dl)++, NATIVE_LEVEL, segment | (waterHidden ? 0x10000 : 0), (first << 16) | (last + 1));
    }
    return next - batch;
}

static __attribute__((noinline)) void nat_planes(const NatView *view) {
    const f32 (*m)[4] = (const f32 (*)[4]) view->matrix;
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
    sPlanesFor = view->serial;
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
        nat_vertex_put(vt, FALSE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, lit ? p->argb : c->argb, offset);
        nat_sq_flush(vt);
    }
    pvr_dr_addr = sq;
}

/** Writes strips to a recording: a line is written whole, so it need not be read first. */
static inline pvr_vertex_t *nat_write_record(pvr_vertex_t *vt, const NatCorner *c, s32 count, u32 offset, s32 lit) {
    const NatCorner *end = c + count;

    for (; c < end; c++, vt++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];

        nat_vertex_put(vt, TRUE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, lit ? p->argb : c->argb, offset);
    }
    return vt;
}

/** The same, with a fade drawn into the frame on the colours (dreamcast/cover.h). */
static __attribute__((noinline, cold)) pvr_vertex_t *nat_write_faded(pvr_vertex_t *record, const NatCorner *c, s32 count,
                                                                     u32 offset, s32 textured) {
    u32 sq = pvr_dr_addr;
    s32 i;

    for (i = 0; i < count; i++, c++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];
        u32 argb = gfx_frame_argb(c->argb, textured);

        if (record != NULL) {
            nat_vertex_put(record, TRUE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, argb, offset);
            record++;
        } else {
            pvr_vertex_t *vt;

            sq ^= 32;
            vt = (pvr_vertex_t *) sq;
            nat_vertex_put(vt, FALSE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, argb, offset);
            nat_sq_flush(vt);
        }
    }
    pvr_dr_addr = sq;
    return record;
}

/** Strips with a position behind the near plane, whose positions are compiled: their matrix is in XMTRX. */
static __attribute__((noinline, cold)) pvr_vertex_t *nat_clip_level(const NatChunk *chunk, const NatCorner *corners, s32 count,
                                                                    s32 cull, pvr_vertex_t *record, f32 scaleX, f32 scaleY,
                                                                    f32 zMul, u32 offset, s32 textured) {
    shz_vec4_t clip[NAT_MAX_POSITIONS];
    s32 i;

    for (i = 0; i < chunk->numPositions; i++) {
        const f32 *p = &chunk->xyz[i * 3];

        clip[i] = shz_xmtrx_transform_vec4(shz_vec4_init(p[0], p[1], p[2], 1.0f));
    }
    return pc_nat_clip(corners, count, clip, NULL, cull, record, scaleX, scaleY, zMul, offset, textured);
}

/**
 * Strips under a scissor that is not on the PVR's tiles, which it would overdraw
 * by up to a tile: split-screen-like views and menus. Triangle by triangle
 * through the scissor. Returns where a recording stands.
 */
static __attribute__((noinline, cold)) pvr_vertex_t *nat_write_exact(pvr_vertex_t *rec, const NatCorner *corners, s32 count, u32 offset,
                                                                     s32 lit, s32 textured, f32 zMul, f32 scaleX, f32 scaleY) {
    GfxTriVert t[3];
    s32 step = 0;
    s32 i;

    gfx_nat_end(rec);
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
    return gfx_nat_position();
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

DKR_NAT_CODE void pc_nat_level_run(const NatView *view, u32 w0, u32 w1) {
    const NatChunk *chunk = &sChunks[w1 >> 16];
    const NatChunk *end = &sChunks[w1 & 0xFFFF];
    const u32 hidden = (w0 & 0x10000) ? NAT_CHUNK_WATER : 0;
    const s32 segment = w0 & 0xFFFF;
    const u32 glare = gDcGlareAdd;
    const s32 fade = gDcFadeOn;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    s32 lastMat = -1;
    pvr_vertex_t *rec = NULL;

    if (sPlanesFor != view->serial) {
        nat_planes(view);
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
            const NatMatInfo *info = &sInfos[chunk->matOne];
            const f32 zMul = info->decal ? 1.003f : 1.0f;
            const u32 offset = info->texture >= 0 ? glare : 0;
            s32 behind = nat_transform(chunk->xyz, chunk->numPositions, sx, sy, tx, ty, zMul);
            const NatCorner *corners = chunk->corners;

            for (part = 0; part < 2; part++) {
                s32 count = part ? chunk->twoSided : chunk->oneSided;
                s32 mat = part ? chunk->matTwo : chunk->matOne;
                // Near-clipped a triangle can become two; through a scissor, five.
                s32 words = (behind || view->exact) ? (15 * count) + 6 : count;

                if (count == 0) {
                    continue;
                }
                if (mat != lastMat || (chunk->flags & NAT_CHUNK_TEX_ANIM)) {
                    pvr_poly_hdr_t animated __attribute__((aligned(32)));
                    const pvr_poly_hdr_t *hdr = &sMats[mat];
                    int ok;

                    if (chunk->flags & NAT_CHUNK_TEX_ANIM) {
                        hdr = nat_anim_header(chunk, segment, mat, &animated);
                    }
                    gfx_nat_end(rec);
                    rec = gfx_nat_header(hdr, info->route, words, &ok);
                    if (!ok) {
                        rec = NULL;
                        lastMat = -1;
                        corners += count;
                        continue;
                    }
                    lastMat = mat;
                } else if (rec != NULL && !gfx_nat_room(rec, words)) {
                    corners += count;
                    continue;
                }
                if (behind) {
                    rec = nat_clip_level(chunk, corners, count, !part, rec, scaleX, scaleY, zMul, offset,
                                         info->texture >= 0);
                } else if (view->exact) {
                    rec = nat_write_exact(rec, corners, count, offset, FALSE, info->texture >= 0, zMul, scaleX, scaleY);
                } else if (fade) {
                    rec = nat_write_faded(rec, corners, count, offset, info->texture >= 0);
                } else if (rec != NULL) {
                    rec = nat_write_record(rec, corners, count, offset, FALSE);
                } else {
                    nat_write_sq(corners, count, offset, FALSE);
                }
                corners += count;
            }
        }
    }
    gfx_nat_end(rec);
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
} NatModel;

#define NAT_MODEL(model) (*(NatModel **) (model)->pad34)
#define NAT_VARIANT_FADING 1
#define NAT_VARIANT_DIRECTIONAL 2
#define NAT_CHUNK_ENVMAP 8

void pc_nat_model_free(ObjectModel *model) {
    NatModel *nat = NAT_MODEL(model);

    if (nat != NULL) {
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
    for (i = 0; i < model->numberOfBatches; i++) {
        nat->chunks[i].corners = (const NatCorner *) (block + nat->chunks[i].cornersAt);
    }
    NAT_MODEL(model) = nat;
}

/** The textures' words in the mats, and which variants can be drawn. */
static __attribute__((noinline, cold)) void nat_model_resolve(const ObjectModel *model, NatModel *nat) {
    u32 i;
    s32 c, v;

    nat->generation = sVramGeneration;
    nat->variants = 15;
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
    }
    for (c = 0; c < model->numberOfBatches; c++) {
        for (v = 0; v < 4; v++) {
            if (nat->chunks[c].mat[v] == 0xFFFF && !(model->batches[c].flags & RENDER_HIDDEN)) {
                nat->variants &= ~(1 << v);
            }
        }
    }
}

s32 pc_nat_model_draw(Gfx **dl, ObjectModel *model, const Vertex *verts, s32 start, s32 flags, s32 overrideVerts) {
    extern s32 cam_get_viewport_layout(void);
    extern s32 gBlockedRenderFlags;
    extern s16 gUseDirectionalLighting;
    NatModel *nat = NAT_MODEL(model);
    s32 variant, i;

    if (nat == NULL || gBlockedRenderFlags != 0 || cam_get_viewport_layout() != 0 || (DKR_NATIVE_SKIP & 2)) {
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
    gDkrNative((*dl)++, NATIVE_MODEL, start | (variant << 8) | (overrideVerts ? 0x400 : 0), verts);
    (*dl)->words.w0 = 0;
    (*dl)->words.w1 = (u32) model;
    (*dl)++;
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
static __attribute__((noinline, cold)) pvr_vertex_t *nat_clip_model(const NatView *view, const Vertex *v, s32 count, s32 split,
                                                                    const NatCorner *corners, s32 numCorners, s32 cull,
                                                                    pvr_vertex_t *record, f32 scaleX, f32 scaleY, f32 zMul,
                                                                    u32 offset, s32 textured) {
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
    return pc_nat_clip(corners, numCorners, clip, argb, cull, record, scaleX, scaleY, zMul, offset, textured);
}

/** A made-up batch's triangles past the near plane, one by one through the clipper. */
static __attribute__((noinline, cold)) pvr_vertex_t *nat_clip_tris(const NatView *view, const Vertex *v, s32 count, const Triangle *tris,
                                                                   s32 numTris, const NatMaterial *mat, pvr_vertex_t *record, f32 scaleX,
                                                                   f32 scaleY, f32 zMul, u32 offset) {
    shz_vec4_t clip[NAT_MAX_POSITIONS];
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    for (i = 0; i < count; i++) {
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
        record = pc_nat_clip(corners, 3, clip, argb, !(t->flags & BACKFACE_DRAW), record, scaleX, scaleY, zMul, offset,
                             mat->textured);
    }
    return record;
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
static __attribute__((noinline, cold)) const pvr_poly_hdr_t *nat_frame_header(const pvr_poly_hdr_t *mat, const TextureHeader *tex,
                                                                              s32 frame, pvr_poly_hdr_t *out) {
    u32 mode2, mode3;

    shz_memcpy32(out, mat, sizeof(*out));
    if (pc_nat_texture_words((const u8 *) tex + frame * tex->textureSize, &mode2, &mode3)) {
        out->mode3 = mode3;
    }
    return out;
}

/** The vertices' colours, from a combiner that does not collapse, or with a fade drawn into the frame. */
static __attribute__((noinline, cold)) void nat_colours_slow(const Vertex *v, s32 count, s32 fast, s32 textured) {
    u32 argb[NAT_MAX_POSITIONS];
    s32 i;

    if (!fast) {
        pc_nat_colours_mux(argb, v, count);
        for (i = 0; i < count; i++) {
            sPoints[i].argb = argb[i];
        }
    }
    if (gDcFadeOn) {
        for (i = 0; i < count; i++) {
            sPoints[i].argb = gfx_frame_argb(sPoints[i].argb, textured);
        }
    }
}

DKR_NAT_CODE void pc_nat_model_run(const NatView *view, u32 w0, u32 w1, u32 w1b) {
    const ObjectModel *model = (const ObjectModel *) w1b;
    const NatModel *nat = NAT_MODEL(model);
    const Vertex *verts = (const Vertex *) w1;
    const s32 variant = (w0 >> 8) & 3;
    const s32 fading = variant & NAT_VARIANT_FADING;
    const s32 overrideVerts = (w0 >> 10) & 1;
    const u32 glare = gDcGlareAdd;
    const s32 fade = gDcFadeOn;
    const NatMatInfo *lastInfo = NULL;
    s32 i = w0 & 0xFF;
    s32 lastMat = -1;
    s32 fast = TRUE;
    u32 keep = 0, konst = 0;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    pvr_vertex_t *rec = NULL;

    // The whole model, at rest, against the view: animation moves its vertices,
    // so the box is taken at twice the size.
    {
        const NatModelHeader *block = nat->block;
        NatChunk box;

        box.centre[0] = block->centre[0];
        box.centre[1] = block->centre[1];
        box.centre[2] = block->centre[2];
        box.half[0] = box.half[1] = box.half[2] = block->half * 2.0f;
        if (sPlanesFor != view->serial) {
            nat_planes(view);
        }
        if (nat_outside(&box)) {
            return;
        }
    }
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
        if (lastInfo == NULL || info->combine0 != lastInfo->combine0 || info->combine1 != lastInfo->combine1 ||
            info->cycle != lastInfo->cycle) {
            fast = pc_nat_classify(info->combine0, info->combine1, info->cycle, view->prim, view->env, &keep, &konst);
            lastInfo = info;
        }
        for (m = 0; m < count; m++) {
            sPoints[m].argb = ((((u32) v[m].a << 24) | ((u32) v[m].r << 16) | ((u32) v[m].g << 8) | (u32) v[m].b) & keep) |
                              konst;
        }
        if (!fast || fade) {
            nat_colours_slow(v, count, fast, info->texture >= 0);
        }
        for (part = 0; part < 2; part++) {
            s32 numCorners = part ? chunk->twoSided : chunk->oneSided;
            s32 mat = chunk->mat[variant] + part;
            s32 words = (behind || view->exact) ? (15 * numCorners) + 6 : numCorners;
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
                int ok;

                if (frame != 0) {
                    hdr = nat_frame_header(hdr, model->textures[info->texture].texture, frame, &framed);
                }
                gfx_nat_end(rec);
                rec = gfx_nat_header(hdr, info->route, words, &ok);
                if (!ok) {
                    rec = NULL;
                    lastMat = -1;
                    corners += numCorners;
                    continue;
                }
                lastMat = frame != 0 ? -1 : mat;
            } else if (rec != NULL && !gfx_nat_room(rec, words)) {
                corners += numCorners;
                continue;
            }
            {
                const NatCorner *c = (chunk->flags & NAT_CHUNK_ENVMAP) ? nat_envmap_corners(model, corners, numCorners, env)
                                                                       : corners;

                if (behind) {
                    rec = nat_clip_model(view, v, count, split, c, numCorners, !part, rec, scaleX, scaleY, zMul, offset,
                                         info->texture >= 0);
                } else if (view->exact) {
                    rec = nat_write_exact(rec, c, numCorners, offset, TRUE, info->texture >= 0, zMul, scaleX, scaleY);
                } else if (rec != NULL) {
                    rec = nat_write_record(rec, c, numCorners, offset, TRUE);
                } else {
                    nat_write_sq(c, numCorners, offset, TRUE);
                }
            }
            corners += numCorners;
        }
    }
    gfx_nat_end(rec);
}

// Materials that are not known offline: a particle's, a shadow's, the water's.
// One is compiled the first time the game asks for it, as its scene starts drawing,
// and kept until the textures change.
#define NAT_MATERIALS 256

static NatMaterial sMaterials[NAT_MATERIALS];

void pc_nat_materials_forget(void) {
    s32 i;

    for (i = 0; i < NAT_MATERIALS; i++) {
        sMaterials[i].used = FALSE;
    }
}

/**
 * As the walk's render-mode code and gfx.c's header compile would come out for
 * these commands. NULL if that takes more than the native path has: no depth test
 * (the 2D layer), or a texture that is not in video memory.
 */
static __attribute__((noinline, cold)) const NatMaterial *nat_material_compile(NatMaterial *m, u32 w0, u32 w1, u32 omh, u32 oml,
                                                                               const TextureHeader *tex, s32 zBuffer, s32 fog) {
    pvr_poly_cxt_t cxt;
    u32 mode2 = 0, mode3 = 0;
    s32 depthTest = zBuffer && (oml & Z_CMP);
    s32 depthWrite = zBuffer && (oml & Z_UPD);
    s32 twoCycle = (omh & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    u32 p = (oml >> (twoCycle ? 28 : 30)) & 3;
    u32 q = (oml >> (twoCycle ? 20 : 22)) & 3;
    s32 blend = (oml & FORCE_BL) && (p == G_BL_CLR_MEM || q == G_BL_CLR_MEM);
    s32 alphaTest = (oml & CVG_X_ALPHA) || (oml & 3) == G_AC_THRESHOLD;
    s32 route;

    if (alphaTest) {
        route = 1;
    } else if (depthTest) {
        route = blend ? 2 : 0;
    } else {
        return NULL;
    }
    if (tex != NULL && !pc_nat_texture_words(tex, &mode2, &mode3)) {
        return NULL;
    }
    pvr_poly_cxt_col(&cxt, route == 0 ? PVR_LIST_OP_POLY : route == 1 ? PVR_LIST_PT_POLY : PVR_LIST_TR_POLY);
    if (tex != NULL) {
        s32 size, uClamp = TRUE, vClamp = TRUE;

        cxt.txr.enable = TRUE;
        cxt.txr.filter = ((omh >> G_MDSFT_TEXTFILT) & 3) == 0 ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        cxt.txr.width = 8;
        cxt.txr.height = 8;
        cxt.txr.format = 0;
        cxt.txr.base = NULL;
        cxt.txr.mipmap = PVR_MIPMAP_DISABLE;
        cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        cxt.txr.uv_flip = PVR_UVFLIP_NONE;
        for (size = 1; size <= 64; size *= 2) {
            uClamp = uClamp && size != tex->width;
            vClamp = vClamp && size != tex->height;
        }
        uClamp = uClamp || (tex->flags & RENDER_CLAMP_X);
        vClamp = vClamp || (tex->flags & RENDER_CLAMP_Y);
        cxt.txr.uv_clamp = uClamp ? (vClamp ? PVR_UVCLAMP_UV : PVR_UVCLAMP_U) : (vClamp ? PVR_UVCLAMP_V : PVR_UVCLAMP_NONE);
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
    if (tex != NULL) {
        f32 uScale, vScale;

        pc_nat_texture_scale(tex, &uScale, &vScale);
        m->uMul = uScale / (32.0f * tex->width);
        m->vMul = vScale / (32.0f * tex->height);
    }
    m->combine0 = w0;
    m->combine1 = w1;
    m->cycle = omh & (3 << G_MDSFT_CYCLETYPE);
    m->otherH = omh;
    m->otherL = oml;
    m->texture = tex;
    m->route = route;
    m->decal = zBuffer && (oml & ZMODE_DEC) == ZMODE_DEC;
    m->textured = tex != NULL;
    m->geometry = (zBuffer ? 1 : 0) | (fog ? 2 : 0);
    m->used = TRUE;
    return m;
}

/** The material for these commands, compiled if it is new. */
static const NatMaterial *nat_material(u32 w0, u32 w1, u32 omh, u32 oml, const TextureHeader *tex, s32 zBuffer, s32 fog) {
    u32 geometry = (zBuffer ? 1 : 0) | (fog ? 2 : 0);
    u32 hash = (w0 * 0x9E3779B1u) ^ (w1 * 0x85EBCA6Bu) ^ (omh * 0xC2B2AE35u) ^ (oml * 0x27D4EB2Fu) ^
               (((u32) tex) * 0x165667B1u) ^ geometry;
    u32 at = (hash ^ (hash >> 15) ^ (hash >> 7)) & (NAT_MATERIALS - 1);
    s32 probe;

    for (probe = 0; probe < 8; probe++) {
        NatMaterial *m = &sMaterials[(at + probe) & (NAT_MATERIALS - 1)];

        if (!m->used) {
            return nat_material_compile(m, w0, w1, omh, oml, tex, zBuffer, fog);
        }
        if (m->combine0 == w0 && m->combine1 == w1 && m->otherH == omh && m->otherL == oml && m->texture == tex &&
            m->geometry == geometry) {
            return m;
        }
    }
    return NULL;
}

const NatMaterial *pc_nat_material(TextureHeader *tex, s32 flags, s32 texOffset) {
    extern const Gfx *material_lookup(TextureHeader *texhead, s32 flags, s32 *zBuffer, s32 *fog);
    extern s32 cam_get_viewport_layout(void);
    const Gfx *entry;
    s32 zBuffer, fog;

    if (cam_get_viewport_layout() != 0 || (DKR_NATIVE_SKIP & 4)) {
        return NULL; // the split-screen scissors: see pc_nat_level_batches
    }
    if (tex != NULL && texOffset && (texOffset < tex->numOfTextures << 8)) {
        tex = (TextureHeader *) ((s8 *) tex + ((texOffset >> 16) * tex->textureSize));
    }
    if (tex != NULL && (TEX_FORMAT(tex->format) == TEX_FORMAT_CI4 || TEX_FORMAT(tex->format) == TEX_FORMAT_CI8)) {
        return NULL;
    }
    entry = material_lookup(tex, flags, &zBuffer, &fog);
    return nat_material(entry[0].words.w0, entry[0].words.w1, entry[1].words.w0 & 0x00FFFFFF, entry[1].words.w1, tex, zBuffer, fog);
}

const NatMaterial *pc_nat_material_state(TextureHeader *tex, const Gfx *state, s32 zBuffer, s32 fog) {
    extern s32 cam_get_viewport_layout(void);

    if (cam_get_viewport_layout() != 0) {
        return NULL;
    }
    return nat_material(state[0].words.w0, state[0].words.w1, state[1].words.w0 & 0x00FFFFFF, state[1].words.w1, tex, zBuffer,
                        fog);
}

void pc_nat_tris(Gfx **dl, const NatMaterial *mat, const Vertex *verts, s32 numVerts, const Triangle *tris, s32 numTris) {
    gDkrNative((*dl)++, NATIVE_TRIS, numVerts | (numTris << 8), verts);
    (*dl)->words.w0 = (u32) mat;
    (*dl)->words.w1 = (u32) tris;
    (*dl)++;
}

/**
 * One batch of what a frame makes up: the walk's blast path, with its
 * material decided. Triangles do not share corners here (their coordinates differ
 * and the batch is short), and are culled on the CPU, so no strip to break.
 */
DKR_NAT_CODE void pc_nat_tris_run(const NatView *view, u32 w0, u32 w1, const NatMaterial *mat, const Triangle *tris) {
    const Vertex *v = (const Vertex *) w1;
    const s32 numVerts = w0 & 0xFF;
    const s32 numTris = (w0 >> 8) & 0xFFF;
    const u32 offset = mat->textured ? gDcGlareAdd : 0;
    const f32 uMul = mat->uMul, vMul = mat->vMul;
    const f32 zMul = mat->decal ? 1.003f : 1.0f;
    f32 scaleX, scaleY;
    f32 sx, sy, tx, ty;
    u32 keep, konst;
    s32 fast, behind, i;
    int ok;
    pvr_vertex_t *rec;
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
    behind = nat_transform_vertices(sPoints, v, numVerts, sx, sy, tx, ty, zMul);
    fast = pc_nat_classify(mat->combine0, mat->combine1, mat->cycle, view->prim, view->env, &keep, &konst);
    for (i = 0; i < numVerts; i++) {
        sPoints[i].argb = ((((u32) v[i].a << 24) | ((u32) v[i].r << 16) | ((u32) v[i].g << 8) | (u32) v[i].b) & keep) | konst;
    }
    if (!fast || gDcFadeOn) {
        nat_colours_slow(v, numVerts, fast, mat->textured);
    }
    rec = gfx_nat_header(&mat->hdr, mat->route, numTris * 15, &ok);
    if (!ok) {
        return;
    }
    if (behind) {
        rec = nat_clip_tris(view, v, numVerts, tris, numTris, mat, rec, scaleX, scaleY, zMul, offset);
        gfx_nat_end(rec);
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
            rec = nat_write_exact(rec, c, 3, offset, TRUE, mat->textured, zMul, scaleX, scaleY);
        }
        gfx_nat_end(rec);
        return;
    }
    sq = pvr_dr_addr;
    for (i = 0; i < numTris; i++) {
        const Triangle *t = &tris[i];
        const NatPoint *p0, *p1, *p2;

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
        if (rec != NULL) {
            nat_vertex_put(rec, TRUE, PVR_CMD_VERTEX, p0->x, p0->y, p0->z, t->uv0.u * uMul, t->uv0.v * vMul, p0->argb, offset);
            nat_vertex_put(rec + 1, TRUE, PVR_CMD_VERTEX, p1->x, p1->y, p1->z, t->uv1.u * uMul, t->uv1.v * vMul, p1->argb, offset);
            nat_vertex_put(rec + 2, TRUE, PVR_CMD_VERTEX_EOL, p2->x, p2->y, p2->z, t->uv2.u * uMul, t->uv2.v * vMul, p2->argb, offset);
            rec += 3;
        } else {
            pvr_vertex_t *o = (pvr_vertex_t *) (sq ^ 32);

            nat_vertex_put(o, FALSE, PVR_CMD_VERTEX, p0->x, p0->y, p0->z, t->uv0.u * uMul, t->uv0.v * vMul, p0->argb, offset);
            nat_sq_flush(o);
            o = (pvr_vertex_t *) sq;
            nat_vertex_put(o, FALSE, PVR_CMD_VERTEX, p1->x, p1->y, p1->z, t->uv1.u * uMul, t->uv1.v * vMul, p1->argb, offset);
            nat_sq_flush(o);
            o = (pvr_vertex_t *) (sq ^ 32);
            nat_vertex_put(o, FALSE, PVR_CMD_VERTEX_EOL, p2->x, p2->y, p2->z, t->uv2.u * uMul, t->uv2.v * vMul, p2->argb, offset);
            nat_sq_flush(o);
            sq ^= 32;
        }
    }
    pvr_dr_addr = sq;
    gfx_nat_end(rec);
}

s32 pc_nat_rect(Gfx **dl, TextureHeader *tex, s32 ulx, s32 uly, s32 lrx, s32 lry, s32 s, s32 t, s32 dsdx, s32 dtdy) {
    u32 mode2, mode3;

    if (TEX_FORMAT(tex->format) == TEX_FORMAT_CI4 || TEX_FORMAT(tex->format) == TEX_FORMAT_CI8 || (DKR_NATIVE_SKIP & 16) ||
        !pc_nat_texture_words(tex, &mode2, &mode3)) {
        return FALSE;
    }
    gDkrNative((*dl)++, NATIVE_RECT, 0, tex);
    (*dl)->words.w0 = ((u32) ulx << 16) | ((u32) uly & 0xFFFF);
    (*dl)->words.w1 = ((u32) lrx << 16) | ((u32) lry & 0xFFFF);
    (*dl)++;
    (*dl)->words.w0 = ((u32) s << 16) | ((u32) t & 0xFFFF);
    (*dl)->words.w1 = ((u32) dsdx << 16) | ((u32) dtdy & 0xFFFF);
    (*dl)++;
    return TRUE;
}

/** A sprite's material for one of its tiles. */
static const NatMaterial *nat_sprite_material(const Gfx *entry, s32 zBuffer, const TextureHeader *tex) {
    return nat_material(entry[0].words.w0, entry[0].words.w1, entry[1].words.w0 & 0x00FFFFFF, entry[1].words.w1, tex, zBuffer, TRUE);
}

s32 pc_nat_sprite(Gfx **dl, const Sprite *sprite, s32 frame, s32 flags, const Vertex *anchor, const Mtx *billboard) {
    extern const Gfx *material_simple_lookup(s32 flags, s32 *zBuffer);
    extern s32 cam_get_viewport_layout(void);
    const u16 *tiles = (const u16 *) &sprite->textures[sprite->numberOfTextures];
    const Gfx *entry;
    s32 zBuffer, i;

    if (cam_get_viewport_layout() != 0 || frame < 0 || frame >= sprite->numberOfFrames || (DKR_NATIVE_SKIP & 8)) {
        return FALSE;
    }
    entry = material_simple_lookup(flags, &zBuffer);
    if (dl == NULL) {
        // Only asking whether it can be done.
        for (i = tiles[frame]; i < tiles[frame + 1]; i++) {
            const TextureHeader *tex = sprite->textures[i];

            if (TEX_FORMAT(tex->format) == TEX_FORMAT_CI4 || TEX_FORMAT(tex->format) == TEX_FORMAT_CI8 ||
                nat_sprite_material(entry, zBuffer, tex) == NULL) {
                return FALSE;
            }
        }
        return TRUE;
    }
    gDkrNative((*dl)++, NATIVE_SPRITE, frame | (zBuffer ? 0x10000 : 0), sprite);
    (*dl)->words.w0 = (u32) anchor;
    (*dl)->words.w1 = (u32) billboard;
    (*dl)++;
    (*dl)->words.w0 = (u32) entry;
    (*dl)->words.w1 = 0;
    (*dl)++;
    return TRUE;
}

/** A sprite's corner past the near plane: through the clipper, the quad as two triangles. */
static __attribute__((noinline, cold)) pvr_vertex_t *nat_clip_quad(const shz_vec4_t *clip, const u32 *argb, const NatCorner *c,
                                                                   pvr_vertex_t *record, f32 scaleX, f32 scaleY, f32 zMul, u32 offset) {
    return pc_nat_clip(c, 4, clip, argb, FALSE, record, scaleX, scaleY, zMul, offset, TRUE);
}

/**
 * A billboard: an anchor in the world, and tiles of four corners each offset from
 * it in clip space through the billboard's own matrix. A vehicle part has no
 * anchor, its corners going through that matrix alone.
 */
DKR_NAT_CODE void pc_nat_sprite_run(const NatView *view, u32 w0, u32 w1, const Gfx *next) {
    const Sprite *sprite = (const Sprite *) w1;
    const Vertex *anchor = (const Vertex *) next[0].words.w0;
    const f32 *billboard = (const f32 *) next[0].words.w1;
    const Gfx *entry = (const Gfx *) next[1].words.w0;
    const s32 frame = w0 & 0xFFFF;
    const s32 zBuffer = (w0 >> 16) & 1;
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
        const NatMaterial *mat = nat_sprite_material(entry, zBuffer, tex);
        const Vertex *v = &verts[i * 4];
        shz_vec4_t clip[4];
        NatCorner c[4];
        u32 argb[4];
        const u32 offset = gDcGlareAdd;
        f32 zMul;
        pvr_vertex_t *rec;
        s32 behind = 0, k;
        int ok;
        f32 uMax, vMax, uMin;

        if (mat == NULL) {
            continue;
        }
        zMul = mat->decal ? 1.003f : 1.0f;
        if (!lastCombine || mat->combine0 != combine0 || mat->combine1 != combine1 || mat->cycle != cycle) {
            fast = pc_nat_classify(mat->combine0, mat->combine1, mat->cycle, view->prim, view->env, &keep, &konst);
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
        if (!fast || gDcFadeOn) {
            nat_colours_slow(v, 4, fast, TRUE);
        }
        for (k = 0; k < 4; k++) {
            argb[k] = sPoints[k].argb;
        }
        uMax = ((tex->width - 1) << 5) * mat->uMul;
        vMax = ((tex->height - 1) << 5) * mat->vMul;
        uMin = mat->uMul; // S10.5: the tile's own one, not quite its edge
        // As sprite_init_frame's two triangles, around the shared diagonal.
        c[0] = (NatCorner) { uMax, 0.0f, 0, PVR_CMD_VERTEX | 1 };
        c[1] = (NatCorner) { uMax, vMax, 0, PVR_CMD_VERTEX | 2 };
        c[2] = (NatCorner) { uMin, 0.0f, 0, PVR_CMD_VERTEX | 0 };
        c[3] = (NatCorner) { uMin, vMax, 0, PVR_CMD_VERTEX_EOL | 3 };
        rec = gfx_nat_header(&mat->hdr, mat->route, (behind || view->exact) ? 36 : 4, &ok);
        if (!ok) {
            continue;
        }
        if (behind) {
            rec = nat_clip_quad(clip, argb, c, rec, scaleX, scaleY, zMul, offset);
        } else if (view->exact) {
            for (k = 0; k < 4; k++) {
                f32 inv = shz_invf_fsrra(clip[k].w);

                sPoints[k].x = (clip[k].x * inv * sx) + tx;
                sPoints[k].y = ty - (clip[k].y * inv * sy);
                sPoints[k].z = inv * zMul;
            }
            rec = nat_write_exact(rec, c, 4, offset, TRUE, TRUE, zMul, scaleX, scaleY);
        } else {
            u32 sq = pvr_dr_addr;

            for (k = 0; k < 4; k++) {
                const shz_vec4_t *p = &clip[c[k].word & 3];
                f32 inv = shz_invf_fsrra(p->w);
                f32 x = (p->x * inv * sx) + tx;
                f32 y = ty - (p->y * inv * sy);
                u32 cmd = c[k].word & 0xF0000000u;
                u32 colour = argb[c[k].word & 3];

                if (rec != NULL) {
                    nat_vertex_put(rec, TRUE, cmd, x, y, inv * zMul, c[k].u, c[k].v, colour, offset);
                    rec++;
                } else {
                    pvr_vertex_t *vt;

                    sq ^= 32;
                    vt = (pvr_vertex_t *) sq;
                    nat_vertex_put(vt, FALSE, cmd, x, y, inv * zMul, c[k].u, c[k].v, colour, offset);
                    nat_sq_flush(vt);
                }
            }
            pvr_dr_addr = sq;
        }
        gfx_nat_end(rec);
    }
}

#if DKR_NATIVE_CHECK
void pc_nat_check_model(Gfx **dl, ObjectModel *model, s32 batch, s32 flags) {
    extern s16 gUseDirectionalLighting;
    NatModel *nat = NAT_MODEL(model);

    if (nat != NULL) {
        if (nat->generation != sVramGeneration) {
            nat_model_resolve(model, nat);
        }
        gDkrNative((*dl)++, NATIVE_CHECK_MODEL,
                   batch | ((flags & RENDER_SEMI_TRANSPARENT) ? 0x100 : 0) | (gUseDirectionalLighting ? 0x200 : 0), model);
    }
}

void pc_nat_check_model_run(u32 w0, u32 w1) {
    const ObjectModel *model = (const ObjectModel *) w1;
    const NatModel *nat = NAT_MODEL(model);
    s32 mat = nat->chunks[w0 & 0xFF].mat[(w0 >> 8) & 3];
    const pvr_poly_hdr_t *hdr;
    u32 got[4];
    s32 route;

    if (mat == 0xFFFF || !(nat->variants & (1 << ((w0 >> 8) & 3)))) {
        return;
    }
    hdr = &nat->mats[mat];
    if (!gfx_nat_last((unsigned int *) got, (int *) &route)) {
        return; // the walk dropped this batch
    }
    sChecked++;
    if ((got[0] & ~(3u << 16)) != (hdr->cmd & ~(3u << 16)) || (got[1] & ~(3u << 27)) != (hdr->mode1 & ~(3u << 27)) ||
        got[2] != hdr->mode2 || (model->batches[w0 & 0xFF].texOffset == 0 && got[3] != hdr->mode3) ||
        route != nat->infos[mat].route) {
        if (nat_check_report(got[2], hdr->mode2, route)) {
            printf("NATCHK model %08lx batch %lu variant %lu header %08lx %08lx %08lx %08lx route %ld, compiled "
                   "%08lx %08lx %08lx %08lx route %d\n",
                   (unsigned long) w1, (unsigned long) (w0 & 0xFF), (unsigned long) ((w0 >> 8) & 3), (unsigned long) got[0],
                   (unsigned long) got[1], (unsigned long) got[2], (unsigned long) got[3], (long) route,
                   (unsigned long) hdr->cmd, (unsigned long) hdr->mode1, (unsigned long) hdr->mode2,
                   (unsigned long) hdr->mode3, nat->infos[mat].route);
        }
    }
}
#endif

#if DKR_NATIVE_CHECK
void pc_nat_check_level(Gfx **dl, s32 segment, s32 batch) {
    if (sLevel != NULL) {
        if (!sResolved) {
            nat_level_resolve();
        }
        gDkrNative((*dl)++, NATIVE_CHECK_LEVEL, segment, batch);
    }
}

/**
 * NATIVE_CHECK=1: the walk has just drawn this batch. Does the compiled form say
 * the same? The culling differs by design: the walk culled on the CPU.
 */
void pc_nat_check_level_run(u32 w0, u32 w1, const u32 *colours, s32 count) {
    const NatSegment *seg = &sSegs[w0 & 0xFFFF];
    const u16 *table = (const u16 *) ((const u8 *) sLevel + seg->tableAt);
    u16 at = table[w1];
    const NatChunk *chunk;
    const pvr_poly_hdr_t *hdr;
    u32 got[4];
    s32 route, i, j, n;

    if (at >= NAT_BATCH_NONE) {
        return;
    }
    chunk = &sChunks[at];
    hdr = &sMats[chunk->matOne];
    if (!gfx_nat_last((unsigned int *) got, (int *) &route)) {
        return; // the walk dropped this batch
    }
    sChecked++;
    if ((got[0] & ~(3u << 16)) != (hdr->cmd & ~(3u << 16)) || (got[1] & ~(3u << 27)) != (hdr->mode1 & ~(3u << 27)) ||
        got[2] != hdr->mode2 || (!(chunk->flags & NAT_CHUNK_TEX_ANIM) && got[3] != hdr->mode3) ||
        route != sInfos[chunk->matOne].route) {
        if (nat_check_report(got[2], hdr->mode2, route)) {
            printf("NATCHK seg %lu batch %lu header %08lx %08lx %08lx %08lx route %ld, compiled %08lx %08lx %08lx %08lx "
                   "route %d\n",
                   (unsigned long) (w0 & 0xFFFF), (unsigned long) w1, (unsigned long) got[0], (unsigned long) got[1],
                   (unsigned long) got[2], (unsigned long) got[3], (long) route, (unsigned long) hdr->cmd,
                   (unsigned long) hdr->mode1, (unsigned long) hdr->mode2, (unsigned long) hdr->mode3,
                   sInfos[chunk->matOne].route);
        }
    }
    n = chunk->oneSided + chunk->twoSided;
    for (i = 0; i < n; i++) {
        for (j = 0; j < count && colours[j] != chunk->corners[i].argb; j++) {}
        if (j == count) {
            if (nat_check_report(chunk->corners[i].argb, 0, route)) {
                printf("NATCHK seg %lu batch %lu colour %08lx not among the walk's %ld\n", (unsigned long) (w0 & 0xFFFF),
                       (unsigned long) w1, (unsigned long) chunk->corners[i].argb, (long) count);
            }
            break;
        }
    }
}
#endif
