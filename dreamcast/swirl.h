#ifndef DKR_DREAMCAST_SWIRL_H
#define DKR_DREAMCAST_SWIRL_H

// The Dreamcast swirl in place of the N64 logo, lit and drawn by the port; see
// dreamcast/swirl.c.

// The display-list marker render_3d_model leaves where the swirl goes.
#define PC_SWIRL_TAG 0xD0CA7F02u

#ifndef _LANGUAGE_ASSEMBLY
// The display-list walk found the marker: draws it.
void pc_swirl_draw(void);
#endif

#endif
