#include "input.h"

#include <SDL2/SDL.h>

// The CONT_* button bits from include/PR/os_cont.h, repeated here because this
// file cannot include the N64 headers (see input.h).
#define BTN_A 0x8000
#define BTN_B 0x4000
#define BTN_Z 0x2000
#define BTN_START 0x1000
#define BTN_UP 0x0800
#define BTN_DOWN 0x0400
#define BTN_LEFT 0x0200
#define BTN_RIGHT 0x0100
#define BTN_L 0x0020
#define BTN_R 0x0010
#define BTN_CUP 0x0008
#define BTN_CDOWN 0x0004
#define BTN_CLEFT 0x0002
#define BTN_CRIGHT 0x0001

// Full deflection on a real stick, and its diagonal component: the N64 stick is
// round, so holding two axes at once cannot reach 80 on both.
#define STICK_MAX 80
#define STICK_DIAG 57

typedef struct {
    int scancode;
    unsigned short button;
} KeyBinding;

static const KeyBinding sKeyBindings[] = {
    { SDL_SCANCODE_X, BTN_A },      // accelerate
    { SDL_SCANCODE_C, BTN_B },      // brake / reverse
    { SDL_SCANCODE_Z, BTN_Z },      // fire weapon
    { SDL_SCANCODE_SPACE, BTN_R },  // hop / powerslide
    { SDL_SCANCODE_Q, BTN_L },
    { SDL_SCANCODE_RETURN, BTN_START },
    { SDL_SCANCODE_I, BTN_CUP },
    { SDL_SCANCODE_K, BTN_CDOWN },
    { SDL_SCANCODE_J, BTN_CLEFT },
    { SDL_SCANCODE_L, BTN_CRIGHT },
};

#define NUM_KEY_BINDINGS (int) (sizeof(sKeyBindings) / sizeof(sKeyBindings[0]))

// Steering: arrow keys and WASD both drive the analog stick.
static int axis_value(const unsigned char *keys, int negA, int negB, int posA, int posB) {
    int value = 0;

    if (keys[negA] || keys[negB]) {
        value -= 1;
    }
    if (keys[posA] || keys[posB]) {
        value += 1;
    }
    return value;
}

void input_host_read(unsigned short *button, signed char *stickX, signed char *stickY) {
    const unsigned char *keys;
    unsigned short buttons = 0;
    int x, y;
    int magnitude;
    int i;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

    // The game polls the controller before the window opens, and SDL_GetKeyboardState
    // is only meaningful once the video subsystem is up.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        return;
    }

    keys = SDL_GetKeyboardState(NULL);

    for (i = 0; i < NUM_KEY_BINDINGS; i++) {
        if (keys[sKeyBindings[i].scancode]) {
            buttons |= sKeyBindings[i].button;
        }
    }

    x = axis_value(keys, SDL_SCANCODE_LEFT, SDL_SCANCODE_A, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D);
    y = axis_value(keys, SDL_SCANCODE_DOWN, SDL_SCANCODE_S, SDL_SCANCODE_UP, SDL_SCANCODE_W);

    magnitude = (x != 0 && y != 0) ? STICK_DIAG : STICK_MAX;

    *button = buttons;
    *stickX = (signed char) (x * magnitude);
    *stickY = (signed char) (y * magnitude);
}
