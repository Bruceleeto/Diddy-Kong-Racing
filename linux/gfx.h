#ifndef LINUX_GFX_H
#define LINUX_GFX_H

// Host graphics layer (linux/gfx.c) — SDL2 + OpenGL.
//
// This is deliberately its own translation unit and uses only plain C types:
// SDL's headers pull in the system's <strings.h>/<stdint.h>, which collide with
// the N64 headers (os_libc.h's bzero prototype, types.h's uintptr_t). So the
// F3DDKR display-list interpreter (linux/main.c) speaks N64 types and talks to
// the host renderer through this interface, and never includes SDL itself.

// A triangle corner, in N64 screen pixels (origin top-left, y down). `z` is the
// perspective-divided depth, negated so that nearer is smaller (GL_LESS).
// `u`/`v` are normalised texture coordinates.
//
// `w` is the clip-space w the vertex was divided by — the camera-space depth.
// The position is already divided, but GL still needs w to interpolate the
// texture coordinates perspective-correctly (see gfx_draw_tris). Screen-space
// geometry that never went through a projection passes w = 1.
typedef struct {
    float x, y, z, w;
    float u, v;
    unsigned char r, g, b, a;
} GfxTriVert;

// Opens the window and GL context. `width`/`height` are the N64 framebuffer
// size the interpreter draws in; the window is that times `scale`.
void gfx_window_init(int width, int height, int scale);

void gfx_frame_begin(void);

// Uploads an RGBA8888 image and returns a handle for gfx_bind_texture().
// `clampS`/`clampT` pick clamp vs. repeat wrapping on each axis.
unsigned int gfx_create_texture(const void *rgba, int width, int height, int clampS, int clampT);

// Binds a texture for subsequent draws. Handle 0 means untextured (the
// triangles are then shaded from vertex colours alone).
void gfx_bind_texture(unsigned int handle);

// Releases a texture created by gfx_create_texture().
void gfx_delete_texture(unsigned int handle);

// Draws `count` vertices as GL_TRIANGLES — i.e. count/3 triangles, Gouraud
// shaded from the vertex colours and modulated by the bound texture.
void gfx_draw_tris(const GfxTriVert *verts, int count);

// Turns depth testing on or off. The 2D overlay (text, HUD, fades) is drawn
// with the z-buffer disabled and relies on display-list order instead, which is
// what the game itself does: it clears G_ZBUFFER before every rectangle.
void gfx_set_depth_test(int enable);

// Presents the frame and pumps the event queue (exits the process on quit).
void gfx_frame_end(void);

#endif
