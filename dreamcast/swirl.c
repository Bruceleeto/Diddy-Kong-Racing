// The Dreamcast swirl in place of the N64 logo on the boot screen.
//
// dreamcast/swirl/make_swirl.py builds the model (build/dc/swirl.model) from
// the public-domain outline of the Dreamcast logo, in the game's object model
// format, coloured orange. object_model_init asks for each model it loads;
// this one is served from here instead of from assets.bin, so the asset image
// and everything laid out from it stay as they are. A European console gets
// the European swirl, in blue.
#include <dc/flashrom.h>
#include <string.h>

#include "types.h"
#include "asset_enums.h"

alignas(32) static const unsigned char sSwirlModel[] = {
#embed "../build/dc/swirl.model"
};

// The orange of the faces and of the edge in make_swirl.py, and the blue.
static const unsigned char sOrangeFace[3] = { 0xED, 0x84, 0x32 };
static const unsigned char sOrangeEdge[3] = { 0xB0, 0x5A, 0x18 };
static const unsigned char sBlueFace[3] = { 0x1E, 0x55, 0xC0 };
static const unsigned char sBlueEdge[3] = { 0x12, 0x35, 0x80 };

/**
 * The size of the model the port supplies for modelID, or 0 to load it from
 * the assets. With out, also copies it there (big-endian, as from the disc).
 */
u32 pc_object_model_override(s32 modelID, void *out) {
    static int sRegion = -1;
    u8 *model = out;
    u32 vertices, count, i;

    if (modelID != ASSET_OBJECTMODEL_N64LOGO) {
        return 0;
    }
    if (out == NULL) {
        return sizeof(sSwirlModel);
    }
    memcpy(out, sSwirlModel, sizeof(sSwirlModel));
    if (sRegion < 0) {
        sRegion = flashrom_get_region();
    }
    if (sRegion != FLASHROM_REGION_EUROPE) {
        return sizeof(sSwirlModel);
    }
    // Vertices are 10 bytes, the colour at 6; the header gives where and how many.
    vertices = ((u32) model[4] << 24) | ((u32) model[5] << 16) | ((u32) model[6] << 8) | model[7];
    count = ((u32) model[0x24] << 8) | model[0x25];
    for (i = 0; i < count; i++) {
        u8 *rgb = model + vertices + i * 10 + 6;

        if (!memcmp(rgb, sOrangeFace, 3)) {
            memcpy(rgb, sBlueFace, 3);
        } else if (!memcmp(rgb, sOrangeEdge, 3)) {
            memcpy(rgb, sBlueEdge, 3);
        }
    }
    return sizeof(sSwirlModel);
}
