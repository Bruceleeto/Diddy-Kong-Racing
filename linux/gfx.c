#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

static SDL_Window *sWindow;
static SDL_GLContext sContext;

void gfx_window_init(int width, int height, int scale) {
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
    // blend. The alpha test drops fully transparent texels so they don't write
    // depth and punch holes in whatever is drawn behind them later.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.05f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

unsigned int gfx_create_texture(const void *rgba, int width, int height, int clampS, int clampT) {
    GLuint id = 0;

    if (sWindow == NULL) {
        return 0;
    }

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    return id;
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

void gfx_frame_begin(void) {
    if (sWindow == NULL) {
        return;
    }
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
