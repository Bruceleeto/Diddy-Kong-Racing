#ifndef DKR_DREAMCAST_COVER_H
#define DKR_DREAMCAST_COVER_H

// Display-list markers around a screen transition's solid part, which the
// port draws in the opaque list (gfx_set_cover in dreamcast/gfx.c).
#define PC_COVER_BEGIN_TAG 0xD0CA7F04u
#define PC_COVER_END_TAG 0xD0CA7F05u

// Display-list markers around a full-screen fade's rect (src/fade_transition.c).
// A fade at partial alpha is announced while the display list is built, so the
// port can draw the whole frame faded instead: every colour before the rect
// scaled by what the fade leaves of it, the fade's colour times its alpha added
// (as for the glare below), and the rect itself left out. Blending colour C at
// alpha a over a pixel is the pixel times 1 - a plus C times a, and done in the
// vertices it costs the PVR nothing, where the rect is a blended polygon over
// every pixel of the screen.
#define PC_FADE_BEGIN_TAG 0xD0CA7F06u
#define PC_FADE_END_TAG 0xD0CA7F07u
void pc_fade_announce(int r, int g, int b, int alpha);

// Display-list markers around the sun glare's rect (src/weather.c lensflare_render).
// Blending colour C at alpha a over a pixel is the pixel times 1 - a, plus C * a.
// Announced while the display list is built, the port draws everything before
// the rect that way, the scaling with the fade's table and C * a added to each
// vertex (in the offset colour where a texture is applied), and leaves the rect
// out: the same picture, without a blended polygon over every pixel.
#define PC_GLARE_BEGIN_TAG 0xD0CA7F08u
#define PC_GLARE_END_TAG 0xD0CA7F09u
void pc_glare_announce(int r, int g, int b, int alpha);

// Before a split-screen view's backdrop (src/tracks.c trackbg_render_gradient): what
// follows is a backdrop again, though the view before it has drawn depth.
#define PC_VIEW_BEGIN_TAG 0xD0CA7F0Au

#endif
