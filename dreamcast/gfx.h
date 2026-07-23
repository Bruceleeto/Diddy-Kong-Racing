#ifndef LINUX_GFX_H
#define LINUX_GFX_H

// Host graphics layer (linux/gfx.c) — SDL2 + OpenGL.
//
// Its own translation unit, plain C types only: SDL's headers pull in the
// system <strings.h>/<stdint.h>, which collide with the N64 headers (os_libc.h's
// bzero prototype, types.h's uintptr_t). The interpreter (linux/main.c) speaks
// N64 types and talks to the renderer through this interface, never including
// SDL itself.

// A triangle corner, in N64 screen pixels (origin top-left, y down). `z` is the
// perspective-divided depth, negated so nearer is smaller (GL_LESS). `u`/`v` are
// normalised texture coordinates.
//
// `w` is the clip-space w the vertex was divided by (camera-space depth). The
// position is already divided, but GL still needs w to interpolate texture
// coordinates perspective-correctly (see gfx_draw_tris). Screen-space geometry
// that never went through projection passes w = 1.
// `fog` is how far the vertex is faded into the fog colour, 0..1: the factor the
// RSP computes and the RDP's blender applies after texturing. Rides in GL's fog
// coordinate; the DC port passes the same number to the PVR in the vertex's
// offset-colour alpha (PVR_FOG_VERTEX).
typedef struct {
    float x, y, z, w;
    float u, v;
    float fog;
    unsigned char r, g, b, a;
} __attribute__((aligned(32))) GfxTriVert;

// Opens the window and GL context. `width`/`height` are the N64 framebuffer
// size the interpreter draws in; the window is that times `scale`.
void gfx_window_init(int width, int height, int scale);

void gfx_frame_begin(void);

// Uploads an RGBA8888 image and returns a handle for gfx_bind_texture().
// `cmS`/`cmT` are the RDP's raw 2-bit clamp/mirror fields for each axis
// (G_TX_WRAP / G_TX_MIRROR / G_TX_CLAMP).
unsigned int gfx_create_texture(const void *rgba, int width, int height, int cmS, int cmT);

// Binds a texture for subsequent draws. Handle 0 means untextured (the
// triangles are then shaded from vertex colours alone).
void gfx_bind_texture(unsigned int handle);

// Point- or bilinear-samples the bound texture (RDP G_TF_POINT vs G_TF_BILERP).
// Call after gfx_bind_texture(): the filter is state on the texture object.
void gfx_set_texture_filter(int point);

// Fades fragments towards `color` by each vertex's fog coordinate. Off for the 2D
// layer and any material without G_FOG.
void gfx_set_fog(int enable, const unsigned char color[4]);

// Texel/vertex-colour combine. `modulate` is texel * colour (3D geometry).
// `blend` lerps from vertex colour to `color` by the texel: a combiner blending
// a texture towards a constant, e.g. the menu's flashing text highlight.
void gfx_set_texenv_blend(const unsigned char color[4]);
void gfx_set_texenv_modulate(void);

// Releases a texture created by gfx_create_texture().
void gfx_delete_texture(unsigned int handle);

// Draws `count` vertices as GL_TRIANGLES — i.e. count/3 triangles, Gouraud
// shaded from the vertex colours and modulated by the bound texture.
void gfx_draw_tris(const GfxTriVert *verts, int count);

// Blast streaming (the native static-geometry path; see gfx.c). Routes and
// submits scissor + header for the current state, and returns the bound
// texture's NPOT UV scales plus the N64-pixels -> framebuffer scale; the caller
// then owns the TA and streams PVR vertices to the store queues
// (pvr_dr_target/pvr_dr_commit). Returns 0 when the state needs a path this
// contract doesn't cover (recorded lists, texenv blend, exact scissor), and the
// caller must use gfx_draw_tris instead.
int gfx_blast_viable(void); // begin() would accept the current state (cheap precheck)
int gfx_blast_begin(float *uScale, float *vScale, float *scaleX, float *scaleY);

// Depth test on/off (RDP Z_CMP). The 2D overlay (text, HUD, fades) draws with
// the z-buffer disabled and relies on display-list order, as the game does: it
// clears G_ZBUFFER before every rectangle.
void gfx_set_depth_test(int enable);

// Depth writes on/off (RDP Z_UPD). Translucent surfaces test against the
// z-buffer without writing, so what's behind them still draws.
void gfx_set_depth_write(int enable);

// Nudges fragments towards the viewer (RDP ZMODE_DEC): a decal (tyre tracks,
// shadows, track markings) sits on the surface under it without z-fighting.
void gfx_set_depth_offset(int enable);

// Discards fragments whose alpha is not greater than `ref` (0..1); the RDP's
// alpha compare. ref 0 still drops fully transparent texels, which would
// otherwise write depth and punch holes in the geometry behind them.
void gfx_set_alpha_test(float ref);

// Clips drawing to a rectangle in N64 screen pixels (origin top-left, y down),
// lower-right corner inclusive; the RDP's G_SETSCISSOR. Confines each player to
// their half in split-screen and clips scrolling text to its box.
void gfx_set_scissor(float x0, float y0, float x1, float y1);

// Drops back to drawing over the whole framebuffer.
void gfx_disable_scissor(void);

// Presents the frame and pumps the event queue (exits the process on quit).
void gfx_frame_end(void);

#endif
