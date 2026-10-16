#ifndef DKR_DREAMCAST_COVER_H
#define DKR_DREAMCAST_COVER_H

// Display-list markers around a screen transition's solid part, which the
// port draws in the opaque list (gfx_set_cover in dreamcast/gfx.c).
#define PC_COVER_BEGIN_TAG 0xD0CA7F04u
#define PC_COVER_END_TAG 0xD0CA7F05u

// Display-list markers around a full-screen fade's rect (src/fade_transition.c).
// A fade to black at partial alpha is announced while the display list is
// built, so the port can draw the whole frame darkened instead: every colour
// before the rect scaled by what the fade leaves of it, and the rect itself
// left out. Blending black at alpha a over a pixel is scaling it by 1 - a, and
// done in the vertices it costs the PVR nothing, where the rect is a blended
// polygon over every pixel of the screen.
#define PC_FADE_BEGIN_TAG 0xD0CA7F06u
#define PC_FADE_END_TAG 0xD0CA7F07u
void pc_fade_announce(int alpha);

#endif
