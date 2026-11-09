// The game's render-mode tables as a host program prints them, for
// build_native_meshes.py: what src/textures_sprites.c hands the RDP for each
// combination of render flags. Built and run during the build; the tables are
// cut out of the game's source by Makefile.dc, so there is one copy of them.
// Each entry is two Gfx commands (combine, other mode), as hexadecimal words.
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "f3ddkr.h"
#include "textures_sprites.h"

extern int printf(const char *format, ...);

#include MATERIAL_TABLES

#define PRINT(table)                                                                     \
    do {                                                                                 \
        unsigned long i, count = sizeof(table) / sizeof(table[0]);                              \
        printf("%s\"%s\": [", first ? "" : ",\n", #table);                               \
        first = 0;                                                                       \
        for (i = 0; i < count; i++) {                                                    \
            printf("%s[%u, %u, %u, %u]", i ? ", " : "", (unsigned) table[i][0].words.w0, \
                   (unsigned) table[i][0].words.w1, (unsigned) table[i][1].words.w0,     \
                   (unsigned) table[i][1].words.w1);                                     \
        }                                                                                \
        printf("]");                                                                     \
    } while (0)

int main(void) {
    int first = 1;

    printf("{");
    PRINT(dRenderSettingsVtxAlpha);
    PRINT(dRenderSettingsSpriteCld);
    PRINT(dRenderSettingsSpriteXlu);
    PRINT(dRenderSettingsCommon);
    PRINT(dRenderSettingsCutout);
    PRINT(dRenderSettingsDecal);
    PRINT(dRenderSettingsSolidColourVtxAlpha);
    PRINT(dRenderSettingsSolidColour);
    PRINT(dRenderSettingsPrimOverlay);
    PRINT(dRenderSettingsDirectionalLighting);
    PRINT(dRenderSettingsBlinkingLights);
    printf("}\n");
    return 0;
}
