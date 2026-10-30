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
#include "native.h"

#define NAT_MAX_POSITIONS 32
#define NAT_CHUNK_WATER 1
#define NAT_CHUNK_TEX_ANIM 2
#define NAT_BATCH_WALK 0xFFFF
#define NAT_BATCH_NONE 0xFFFE

#ifndef DKR_NATIVE_CHECK
#define DKR_NATIVE_CHECK 0
#endif

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

// A frame's five planes through the view: the box is outside when its corner
// nearest a plane's positive side lies behind it.
typedef struct {
    f32 n[3], d;
    f32 abs[3], pad;
} NatPlane;

static DKR_HOT NatPlane sPlanes[5];
static DKR_HOT s32 sPlanesFor = -1;

typedef struct {
    f32 x, y, z, pad;
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

/** Reads the level's block from the pack. Without it the walk draws the level, as it can. */
void pc_nat_level_load(s32 modelId) {
    static file_t sPack = FILEHND_INVALID;
    static u32 *sIndex;
    static u32 sLevels;
    static u32 sCount; // level models, then object models
    u8 *block;
    u32 bytes;
    u32 i;

    pc_nat_level_free();
    if (sPack == FILEHND_INVALID) {
        u32 head[4];

        sPack = fs_open(DKR_ASSET_MOUNT "/build/dc/native.pack", O_RDONLY);
        if (sPack == FILEHND_INVALID) {
            printf("native: no mesh pack\n");
            return;
        }
        if (fs_read(sPack, head, 16) != 16 || head[0] != 0x4B504E44 /* DNPK */ || head[1] != 2) {
            printf("native: mesh pack is not version 2\n");
            fs_close(sPack);
            sPack = FILEHND_INVALID;
            return;
        }
        sLevels = head[2];
        sCount = head[2] + head[3];
        sIndex = malloc(sCount * 8);
        if (sIndex == NULL || fs_read(sPack, sIndex, sCount * 8) != (ssize_t) (sCount * 8)) {
            printf("native: mesh pack index unread\n");
            sCount = sLevels = 0;
            return;
        }
    }
    if ((u32) modelId >= sLevels || sIndex[modelId * 2 + 1] == 0) {
        return;
    }
    bytes = sIndex[modelId * 2 + 1];
    block = memalign(32, bytes);
    if (block == NULL) {
        printf("native: malloc failed, %lu bytes for level model %ld\n", (unsigned long) bytes, (long) modelId);
        return;
    }
    if (fs_seek(sPack, sIndex[modelId * 2], SEEK_SET) < 0 || fs_read(sPack, block, bytes) != (ssize_t) bytes) {
        printf("native: level model %ld unread\n", (long) modelId);
        free(block);
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

    if (sLevel == NULL) {
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
static inline void nat_write_sq(const NatCorner *c, s32 count, u32 offset) {
    u32 sq = pvr_dr_addr;
    const NatCorner *end = c + count;

    for (; c < end; c++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];
        pvr_vertex_t *vt;

        sq ^= 32;
        vt = (pvr_vertex_t *) sq;
        nat_vertex_put(vt, FALSE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, c->argb, offset);
        nat_sq_flush(vt);
    }
    pvr_dr_addr = sq;
}

/** Writes strips to a recording: a line is written whole, so it need not be read first. */
static inline pvr_vertex_t *nat_write_record(pvr_vertex_t *vt, const NatCorner *c, s32 count, u32 offset) {
    const NatCorner *end = c + count;

    for (; c < end; c++, vt++) {
        const NatPoint *p = &sPoints[c->word & 0xFFFF];

        nat_vertex_put(vt, TRUE, c->word & 0xF0000000u, p->x, p->y, p->z, c->u, c->v, c->argb, offset);
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
                s32 words = behind ? (6 * count) + 6 : count; // near-clipped, a strip's triangle can become two

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
                    rec = pc_nat_clip(corners, count, chunk->xyz, !part, rec, scaleX, scaleY, zMul, offset,
                                      info->texture >= 0);
                } else if (fade) {
                    rec = nat_write_faded(rec, corners, count, offset, info->texture >= 0);
                } else if (rec != NULL) {
                    rec = nat_write_record(rec, corners, count, offset);
                } else {
                    nat_write_sq(corners, count, offset);
                }
                corners += count;
            }
        }
    }
    gfx_nat_end(rec);
}

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
    static u32 sChecked, sBad;
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
    gfx_nat_last(got, &route);
    sChecked++;
    if ((got[0] & ~(3u << 16)) != (hdr->cmd & ~(3u << 16)) || (got[1] & ~(3u << 27)) != (hdr->mode1 & ~(3u << 27)) ||
        got[2] != hdr->mode2 || (!(chunk->flags & NAT_CHUNK_TEX_ANIM) && got[3] != hdr->mode3) ||
        route != sInfos[chunk->matOne].route) {
        if (sBad++ < 40) {
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
            if (sBad++ < 40) {
                printf("NATCHK seg %lu batch %lu colour %08lx not among the walk's %ld\n", (unsigned long) (w0 & 0xFFFF),
                       (unsigned long) w1, (unsigned long) chunk->corners[i].argb, (long) count);
            }
            break;
        }
    }
    if ((sChecked & 0xFFFFF) == 0) {
        printf("NATCHK %lu batches, %lu differ\n", (unsigned long) sChecked, (unsigned long) sBad);
    }
}
#endif
