#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

static SDL_Window *sWindow;
static SDL_GLContext sContext;

// The N64 framebuffer size we draw in, and how many host pixels one of ours is.
// gfx_set_scissor() needs both: GL's scissor is in window pixels, measured from
// the bottom-left, while the game speaks 320x240 from the top-left.
static int sFbWidth = 320;
static int sFbHeight = 240;
static int sScale = 1;

void gfx_window_init(int width, int height, int scale) {
    sFbWidth = width;
    sFbHeight = height;
    sScale = scale;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    sWindow = SDL_CreateWindow("Diddy Kong Racing", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * scale,
                               height * scale, SDL_WINDOW_OPENGL);
    if (sWindow == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }

    sContext = SDL_GL_CreateContext(sWindow);
    if (sContext == NULL) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return;
    }
    SDL_GL_SetSwapInterval(0); // pc_retrace_wait() already paces the game

    glViewport(0, 0, width * scale, height * scale);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Draw straight in N64 screen coordinates: origin top-left, y downwards.
    // The vertices arrive already projected and divided, so z is just a depth
    // value in [-1, 1].
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glShadeModel(GL_SMOOTH);

    // Texture alpha is how the N64 cuts out sprites and foliage, so it has to
    // blend. Depth test, depth write and the alpha-compare threshold are all
    // driven per material now (see apply_render_mode in main.c); these are just
    // the startup defaults.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.0f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // Nearer is smaller here, so a decal has to be pulled towards zero.
    glPolygonOffset(-1.0f, -1.0f);
}

// The RDP's cms/cmt are a 2-bit field, not a flag: bit 0 is mirror, bit 1 is
// clamp (G_TX_MIRROR = 1, G_TX_CLAMP = 2, plain wrap = 0). Clamp wins if both.
static GLint wrap_mode(int cm) {
    if (cm & 0x2) {
        return GL_CLAMP_TO_EDGE;
    }
    if (cm & 0x1) {
        return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

unsigned int gfx_create_texture(const void *rgba, int width, int height, int cmS, int cmT) {
    GLuint id = 0;

    if (sWindow == NULL) {
        return 0;
    }

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(cmS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(cmT));

    return id;
}

/**
 * Texel-weighted lerp between the vertex colour and `color` — GL's GL_BLEND
 * texture env, which computes Cf*(1 - Ct) + Cc*Ct per channel (and Af*At for
 * alpha, same as modulate).
 *
 * That is the exact shape of a combiner that blends the texel towards a constant.
 * Hand it the combiner evaluated with no texel as the vertex colour and with a
 * white texel as `color`, and the hardware reconstructs the real result. All GL
 * 1.1 — no secondary colour, no GL_COMBINE.
 */
void gfx_set_texenv_blend(const unsigned char color[4]) {
    GLfloat c[4];

    if (sWindow == NULL) {
        return;
    }

    c[0] = color[0] / 255.0f;
    c[1] = color[1] / 255.0f;
    c[2] = color[2] / 255.0f;
    c[3] = color[3] / 255.0f;

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
}

/** Plain texel * vertex colour, which is what the 3D path wants. */
void gfx_set_texenv_modulate(void) {
    if (sWindow == NULL) {
        return;
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

void gfx_set_texture_filter(int point) {
    GLint filter;

    if (sWindow == NULL) {
        return;
    }

    filter = point ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void gfx_bind_texture(unsigned int handle) {
    if (sWindow == NULL) {
        return;
    }

    if (handle == 0) {
        glDisable(GL_TEXTURE_2D);
    } else {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, handle);
    }
}

// `x1`/`y1` are exclusive — the caller has already turned the RDP's inclusive
// lower-right corner into one past the end.
void gfx_set_scissor(float x0, float y0, float x1, float y1) {
    int w, h, gx, gy;

    if (sWindow == NULL) {
        return;
    }

    // GL measures from the bottom-left, so y flips.
    gx = (int) (x0 * sScale);
    gy = (int) ((sFbHeight - y1) * sScale);
    w = (int) ((x1 - x0) * sScale);
    h = (int) ((y1 - y0) * sScale);

    if (w < 0 || h < 0) {
        // An empty rect means draw nothing, which is not the same as "no clip".
        w = 0;
        h = 0;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(gx, gy, w, h);
}

void gfx_disable_scissor(void) {
    if (sWindow == NULL) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
}

void gfx_frame_begin(void) {
    if (sWindow == NULL) {
        return;
    }
    // glClear honours both the depth mask and the scissor, so neither may be left
    // where the previous frame's last command put it, or the clear silently does
    // nothing (or only part of the screen).
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gfx_draw_tris(const GfxTriVert *verts, int count) {
    int i;

    if (sWindow == NULL || count <= 0) {
        return;
    }

    // The vertices arrive already divided by w, but handing GL a position with
    // an implicit w of 1 would make it interpolate the texture coordinates
    // linearly in screen space — affine mapping, which visibly swims and shears
    // on a polygon whose corners are at very different depths (a wall up close).
    // The N64 does not do that: G_TP_PERSP is set in every DKR othermode, and
    // the RDP interpolates against 1/w per pixel.
    //
    // So multiply the position back up by w and hand GL the real w. The
    // projection is a plain ortho, so the divide GL does reproduces exactly the
    // screen position computed in project() — but now w is on the wire, and the
    // fixed-function rasteriser interpolates the texture perspective-correctly.
    glBegin(GL_TRIANGLES);
    for (i = 0; i < count; i++) {
        float w = verts[i].w;

        glColor4ub(verts[i].r, verts[i].g, verts[i].b, verts[i].a);
        glTexCoord2f(verts[i].u, verts[i].v);
        glVertex4f(verts[i].x * w, verts[i].y * w, verts[i].z * w, w);
    }
    glEnd();
}

void gfx_delete_texture(unsigned int handle) {
    GLuint id = handle;

    if (sWindow == NULL || handle == 0) {
        return;
    }
    glDeleteTextures(1, &id);
}

void gfx_set_depth_test(int enable) {
    if (sWindow == NULL) {
        return;
    }

    if (enable) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

void gfx_set_depth_write(int enable) {
    if (sWindow == NULL) {
        return;
    }
    glDepthMask(enable ? GL_TRUE : GL_FALSE);
}

void gfx_set_depth_offset(int enable) {
    if (sWindow == NULL) {
        return;
    }

    if (enable) {
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

void gfx_set_alpha_test(float ref) {
    if (sWindow == NULL) {
        return;
    }
    glAlphaFunc(GL_GREATER, ref);
}

void gfx_frame_end(void) {
    SDL_Event event;

    if (sWindow == NULL) {
        return;
    }

    SDL_GL_SwapWindow(sWindow);

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
            SDL_Quit();
            exit(0);
        }
    }
}
