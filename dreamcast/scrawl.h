#ifndef DKR_DREAMCAST_SCRAWL_H
#define DKR_DREAMCAST_SCRAWL_H

// "n't" scratched into the title screen's beach after the Nintendo logo, bump
// mapped; see dreamcast/scrawl.c.

// The display-list marker the level draw leaves where it goes.
#define PC_SCRAWL_TAG 0xD0CA7F01u

#ifndef _LANGUAGE_ASSEMBLY
struct Gfx;
// After the level draws a batch: leaves the marker after the logo's.
void pc_scrawl_after_batch(int segment, int batch, void *dl);
// The display-list walk found the marker: draws it.
void pc_scrawl_draw(void);
#endif

#endif
