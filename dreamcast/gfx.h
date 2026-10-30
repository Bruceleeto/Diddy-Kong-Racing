#ifndef LINUX_GFX_H
#define LINUX_GFX_H

// The native renderer's hot code (dreamcast/native.c): together, away from the
// walk's, so that what the walk still does and what it no longer does can be
// told apart.
#define DKR_NAT_CODE __attribute__((section(".text.dkr_native")))

// The state every batch reads, in the display-list walk (main.c) and here, is
// some sixty small variables. Left to the linker they land in as many cache
// lines, scattered among the big tables, and the operand cache is small and
// direct-mapped: the vertex and recording traffic of a batch evicts them, and
// the next batch misses on each one again. Marked with this they share one
// section, a handful of lines.
#define DKR_HOT __attribute__((section(".data.dkr_hot")))
// The same for the code a batch runs through. The instruction cache is 8 KB
// and direct-mapped too, and the walk's handlers, left where the linker puts
// them, overlapped in it: batch after batch they evicted each other, and any
// unrelated change that moved them made the frame faster or slower. KOS's
// linker script gives .sub0 an address aligned to the cache size and fails
// the link if it outgrows one cache's worth, so the hot handlers sit at a fixed
// place in the cache and never on top of each other. Their cold paths are
// split out of them to keep it that way.
#if defined(DKR_HOTLAYOUT) && DKR_HOTLAYOUT
// HOTLAYOUT=1: each hot function keeps its own .text.<name> section, which the
// ordering file from dreamcast/hotlayout.tsv places at a fixed slot.
#define DKR_HOT_CODE
#elif (defined(DKR_GFX_PROFILE) && DKR_GFX_PROFILE == 2) || (defined(DKR_FILL_PROF) && DKR_FILL_PROF)
// The per-handler timers, and the fill profile's per-vertex calls, do not fit
// in one cache's worth; they disturb the timing anyway. Kept together all the
// same.
#define DKR_HOT_CODE __attribute__((section(".text.dkr_hot")))
#else
#define DKR_HOT_CODE __attribute__((section(".sub0.dkr_hot")))
#endif
// The hot handler that leaves .sub0 in a frame-profile build (GFX_PROFILE=1,
// 3, 4): the profile's counters in the others add about 1.3 KB, 24 bytes more
// than the cache holds. Its layout differs from the game's build anyway.
#if defined(DKR_GFX_PROFILE) && DKR_GFX_PROFILE && !(defined(DKR_HOTLAYOUT) && DKR_HOTLAYOUT)
#define DKR_HOT_CODE_SPARE __attribute__((section(".text.dkr_hot")))
#else
#define DKR_HOT_CODE_SPARE DKR_HOT_CODE
#endif

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
unsigned int gfx_adopt_texture(void *data, int width, int height, int pw, int ph, unsigned int format,
                               int mipmapped, int kind);
void gfx_texture_set_wrap(unsigned int handle, int cmS, int cmT);

// Binds a texture for subsequent draws. Handle 0 means untextured (the
// triangles are then shaded from vertex colours alone).
void gfx_bind_texture(unsigned int handle);

// Point- or bilinear-samples the bound texture (RDP G_TF_POINT vs G_TF_BILERP).
// Call after gfx_bind_texture(): the filter is state on the texture object.
void gfx_set_texture_filter(int point);

// Fades fragments towards `color` by each vertex's fog coordinate. Off for the 2D
// layer and any material without G_FOG.
void gfx_set_fog(int enable, const unsigned char color[4]);

// Where the fog lies: the RSP's fog factor, 0..255, is c0 + c1 / w for a vertex
// at clip-space w, and maxW is w at the far plane. It becomes the PVR's fog
// table, so fog costs no CPU per vertex.
void gfx_set_fog_depth(float c0, float c1, float maxW);

// For a native draw that wants fog of its own (dreamcast/swirl.c): this frame's
// fog is rgb, none at nearW and full at farW, linear in w, instead of the game's.
// Its headers ask for PVR_FOG_TABLE themselves; the game's own fog is off meanwhile.
void gfx_fog_linear(unsigned int rgb, float nearW, float farW);

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
// texture's NPOT UV scales plus the N64-pixels -> framebuffer scale. The caller
// then writes at most `maxWords` PVR vertices: to the store queues
// (pvr_dr_target/pvr_dr_commit) when *record is NULL, otherwise consecutively
// from *record, passing where it stopped to gfx_blast_end(). Returns 0 when the
// state needs a path this contract doesn't cover (punch-through, texenv blend,
// exact scissor), and the caller must use gfx_draw_tris instead.
int gfx_blast_viable(void); // begin() would accept the current state (cheap precheck)
int gfx_blast_begin(float *uScale, float *vScale, float *scaleX, float *scaleY, int maxWords,
                    void **record, int emitAlpha, float *zMul, float *zAdd);
void gfx_blast_end(void *recordEnd);

// TA words recorded so far this frame for the punch-through and translucent
// lists (32 bytes each; a triangle is three). For the frame profile.
void gfx_recorded_words(int *pt, int *tr);

// The native renderer (dreamcast/native.c). Opens a run of vertices under a
// compiled header in the list `route` (0 opaque, 1 punch-through, 2 translucent).
// Returns the recording to write up to `words` vertices to, or NULL for the store
// queues, the opaque list being streamed; *ok is 0 when the list has no room.
void *gfx_nat_header(const void *hdr, int route, int words, int *ok);
void gfx_nat_end(void *recordEnd);
int gfx_nat_room(void *record, int words); // room for `words` more in the open recording
void gfx_nat_scale(float *x, float *y);
// Whether the scissor can be left to the PVR's tiles alone, as native draws do.
int gfx_nat_viable(void);
// The bits of a polygon header that come from a texture.
int gfx_texture_words(unsigned int handle, unsigned int *mode2, unsigned int *mode3);
// What the blast path last sent, for NATIVE_CHECK=1.
void gfx_nat_last(unsigned int words[4], int *route);

// A native polygon in the translucent list, at this point of the display list
// (dreamcast/scrawl.c). Vertex x/y are N64 pixels. 0 if the recording is full.
#include <dc/pvr.h>
int gfx_tr_native(const pvr_poly_hdr_t *hdr, const pvr_vertex_t *verts, int count);
// The same in the opaque list, streamed as it comes (dreamcast/swirl.c).
int gfx_op_native(const pvr_poly_hdr_t *hdr, const pvr_vertex_t *verts, int count);

// Every colour drawn from here on scaled by keep / 255 (red, green, blue; not
// alpha), for a fade to black drawn into the frame (dreamcast/cover.h). 255
// draws colours as they are.
void gfx_set_fade(int keep);
extern int gDcFadeOn;
extern unsigned char gDcFadeLut[256];

static inline unsigned int gfx_fade_argb(unsigned int c) {
    if (!gDcFadeOn) {
        return c;
    }
    return (c & 0xFF000000u) | ((unsigned int) gDcFadeLut[(c >> 16) & 0xFF] << 16) |
           ((unsigned int) gDcFadeLut[(c >> 8) & 0xFF] << 8) | gDcFadeLut[c & 0xFF];
}

// What the sun glare and a coloured fade drawn into the frame add to every
// colour (dreamcast/cover.h): each one's colour times its alpha, the glare's
// also times the fade drawn after it, as 0x00RRGGBB; 0 for none. Their scaling
// part is in the fade table above. gfx_set_glare sets both; gfx_set_fade
// clears the addition.
void gfx_set_glare(int keep, unsigned int add);
extern unsigned int gDcGlareAdd;

/** Each colour channel of c plus add's, saturating; c's alpha is kept. */
static inline unsigned int gfx_add_rgb(unsigned int c, unsigned int add) {
    unsigned int r = ((c >> 16) & 0xFF) + ((add >> 16) & 0xFF);
    unsigned int g = ((c >> 8) & 0xFF) + ((add >> 8) & 0xFF);
    unsigned int b = (c & 0xFF) + (add & 0xFF);

    return (c & 0xFF000000u) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

/**
 * A vertex colour as this frame draws it: faded, and for an untextured polygon
 * also the glare added (a textured one carries the glare in its offset colour,
 * gfx_frame_oargb, which the PVR adds after the texture).
 */
static inline unsigned int gfx_frame_argb(unsigned int c, int textured) {
    c = gfx_fade_argb(c);
    return (gDcGlareAdd && !textured) ? gfx_add_rgb(c, gDcGlareAdd) : c;
}

/** A textured polygon's offset colour as this frame draws it: faded, plus the glare. */
static inline unsigned int gfx_frame_oargb(unsigned int o) {
    o = gfx_fade_argb(o);
    return gDcGlareAdd ? gfx_add_rgb(o, gDcGlareAdd) : o;
}

// While set, opaque untextured 2D batches are a cover over the whole scene,
// drawn in the opaque list; see gfx.c. The display list sets it (PC_COVER_*).
void gfx_set_cover(int enable);

// Fill profile (FILL_PROF=1, see gfx.c): the display list's category, one
// vertex as it goes out in framebuffer pixels, the start of each frame's
// walk, and the per-scene report.
void gfx_prof_set_category(int cat);
void gfx_prof_fill_vert(float x, float y, int eol);
void gfx_fill_frame(int counted);
void gfx_fill_frame_done(unsigned int renderUs);
void gfx_fill_report(void);
void gfx_prof_set_skip(int code);

// Whether the surface blends with what is behind it (RDP FORCE_BL). One that
// does not is opaque whatever its alpha says, and goes to the opaque list.
void gfx_set_blend(int enable);

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
// ENJ_RENDER=1: submit a frame through enDjinn's render lists, calling `walk`
// inside the opaque list where gfx_frame_begin/gfx_frame_end would bracket it.
void gfx_frame_render(void (*walk)(void *), void *data);
// ENJ_LOOP=1: queue the same renderers for enDjinn's loop to submit, and finish
// the frame once it has (enDjinn's post-render callback).
void gfx_frame_queue(void (*walk)(void *), void *data);
void gfx_frame_submitted(void);

#endif
