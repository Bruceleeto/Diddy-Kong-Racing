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

    glBegin(GL_TRIANGLES);
    for (i = 0; i < count; i++) {
        glColor3ub(verts[i].r, verts[i].g, verts[i].b);
        glVertex3f(verts[i].x, verts[i].y, verts[i].z);
    }
    glEnd();
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
