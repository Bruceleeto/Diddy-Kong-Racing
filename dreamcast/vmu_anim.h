#ifndef DREAMCAST_VMU_ANIM_H
#define DREAMCAST_VMU_ANIM_H

#include <stdint.h>
#include <kos.h>
#include <dc/vmu_fb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMU_ANIM_FRAME_COUNT 252
#define VMU_ANIM_FRAME_BYTES 192

extern const uint8_t gVmuAnimFrames[VMU_ANIM_FRAME_COUNT][VMU_ANIM_FRAME_BYTES];

void vmu_anim_update(void);

#ifdef __cplusplus
}
#endif

#endif // DREAMCAST_VMU_ANIM_H
