#ifndef _COLLISION_H_
#define _COLLISION_H_

#include "types.h"
#include "structs.h"

#define MAX_COLLISION_CANDIDATES 500

// gCollisionCandidates entries are tagged pointers: segment entries vs
// CollisionFacetPlanes entries. Retail's asm tagged via the KSEG0 sign bit
// (segments stored as positive physical addresses), which host pointers don't
// have. Since collision.c is now the producer on every target, segments are
// tagged with bit 0 instead — all candidates are at least 4-byte aligned, so
// the bit is free on N64 and PC alike.
#define COLLISION_SEGMENT_ENTRY(seg) (((s32) (seg)) | 1)
#define COLLISION_ENTRY_IS_SEGMENT(entry) ((entry) & 1)
#define COLLISION_ENTRY_SEGMENT(entry) ((LevelModelSegment *) ((entry) & ~1))

enum CollisionModes { COLLISION_MODE_DEFAULT, COLLISION_MODE_1, COLLISION_MODE_NO_WALLS };

void generate_collision_candidates(s32 numPoints, Vec3f *origins, Vec3f *targets, s32 vehicleID);
s32 compute_grid_overlap_mask(LevelModelSegmentBoundingBox *bbox, s32 x1, s32 z1, s32 x2, s32 z2);
s32 resolve_collisions(Vec3f *origin, Vec3f *target, f32 *radius, s8 *surface, s32 numEntries, s32 *numCollisions);

#endif
