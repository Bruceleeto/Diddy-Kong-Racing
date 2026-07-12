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
typedef struct {
    float x, y, z;
    unsigned char r, g, b;
} GfxTriVert;

// Opens the window and GL context. `width`/`height` are the N64 framebuffer
// size the interpreter draws in; the window is that times `scale`.
void gfx_window_init(int width, int height, int scale);

void gfx_frame_begin(void);

// Draws `count` vertices as GL_TRIANGLES — i.e. count/3 triangles, Gouraud
// shaded from the vertex colours.
void gfx_draw_tris(const GfxTriVert *verts, int count);

// Presents the frame and pumps the event queue (exits the process on quit).
void gfx_frame_end(void);

#endif
