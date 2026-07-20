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

// The game only ever looks at four ports (MAXCONTROLLERS).
#define MAX_PORTS 4

// Full deflection on a real stick, and its diagonal component: the N64 stick is
// round, so holding two axes at once cannot reach 80 on both.
#define STICK_MAX 80
#define STICK_DIAG 57

// Analog stick noise floor, in SDL's -32768..32767 axis range.
#define PAD_DEADZONE 8000

// An analog trigger past this (SDL reports 0..32767) counts as a digital press.
#define PAD_TRIG_THRESHOLD 8000

#define ARRAY_LEN(a) (int) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int scancode;
    unsigned short button;
} KeyBinding;

// Two keyboard players, so split screen is reachable without owning a pad.
// Port 0 keeps the original layout; port 1 lives on the numpad.
static const KeyBinding sKeyBindingsP1[] = {
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

static const KeyBinding sKeyBindingsP2[] = {
    { SDL_SCANCODE_KP_0, BTN_A },         // accelerate
    { SDL_SCANCODE_KP_PERIOD, BTN_B },    // brake / reverse
    { SDL_SCANCODE_KP_PLUS, BTN_Z },      // fire weapon
    { SDL_SCANCODE_KP_MINUS, BTN_R },     // hop / powerslide
    { SDL_SCANCODE_KP_MULTIPLY, BTN_L },
    { SDL_SCANCODE_KP_ENTER, BTN_START },
    { SDL_SCANCODE_KP_5, BTN_CUP },
    { SDL_SCANCODE_KP_1, BTN_CDOWN },
    { SDL_SCANCODE_KP_7, BTN_CLEFT },
    { SDL_SCANCODE_KP_9, BTN_CRIGHT },
};

// Steering for each keyboard player. P1 takes arrows and WASD, P2 the numpad,
// which has only one key per direction — hence the doubled scancodes.
typedef struct {
    const KeyBinding *bindings;
    int numBindings;
    int leftA, leftB, rightA, rightB;
    int downA, downB, upA, upB;
} KeyboardPlayer;

static const KeyboardPlayer sKeyboardPlayers[] = {
    { sKeyBindingsP1, ARRAY_LEN(sKeyBindingsP1),
      SDL_SCANCODE_LEFT, SDL_SCANCODE_A, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D,
      SDL_SCANCODE_DOWN, SDL_SCANCODE_S, SDL_SCANCODE_UP, SDL_SCANCODE_W },
    { sKeyBindingsP2, ARRAY_LEN(sKeyBindingsP2),
      SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_6,
      SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_8 },
};

// Gamepad slot N drives port N, so a lone pad is player 1 and both keyboard
// layouts stay live alongside whatever is plugged in.
static SDL_GameController *sPads[MAX_PORTS];
static int sLastJoystickCount = -1;

// Reopen the pad list when the number of attached joysticks changes. Closing
// and reopening everything on a hotplug keeps port assignment deterministic
// (the first pad SDL lists is always port 0) at the cost of a blip on the frame
// a pad is added or removed.
static void pads_refresh(void) {
    int count, i, slot;

    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER) && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        return;
    }

    count = SDL_NumJoysticks();
    if (count != sLastJoystickCount) {
        sLastJoystickCount = count;

        for (i = 0; i < MAX_PORTS; i++) {
            if (sPads[i] != NULL) {
                SDL_GameControllerClose(sPads[i]);
                sPads[i] = NULL;
            }
        }

        slot = 0;
        for (i = 0; i < count && slot < MAX_PORTS; i++) {
            if (!SDL_IsGameController(i)) {
                continue; // a joystick SDL has no button mapping for
            }
            sPads[slot] = SDL_GameControllerOpen(i);
            if (sPads[slot] != NULL) {
                slot++;
            }
        }
    }

    SDL_GameControllerUpdate();
}

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

// Scale one SDL analog axis into the N64's range, squashing the deadzone.
static int pad_axis(SDL_GameController *pad, SDL_GameControllerAxis axis) {
    int raw = SDL_GameControllerGetAxis(pad, axis);

    if (raw > -PAD_DEADZONE && raw < PAD_DEADZONE) {
        return 0;
    }
    raw = (raw * STICK_MAX) / 32767;
    if (raw < -STICK_MAX) {
        raw = -STICK_MAX;
    }
    if (raw > STICK_MAX) {
        raw = STICK_MAX;
    }
    return raw;
}

unsigned int input_host_port_mask(void) {
    unsigned int mask;
    int i;

    // Both keyboard layouts are always live, so the game always sees two ports
    // and a second player can join at character select on any machine.
    mask = 0x3;

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        pads_refresh();
        for (i = 0; i < MAX_PORTS; i++) {
            if (sPads[i] != NULL) {
                mask |= 1u << i;
            }
        }
    }
    return mask;
}

void input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY) {
    const unsigned char *keys;
    const KeyboardPlayer *kb;
    SDL_GameController *pad;
    unsigned short buttons = 0;
    int x = 0, y = 0;
    int magnitude;
    int i;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

    if (port < 0 || port >= MAX_PORTS) {
        return;
    }

    // The game polls the controller before the window opens, and
    // SDL_GetKeyboardState is only meaningful once the video subsystem is up.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        return;
    }

    if (port < ARRAY_LEN(sKeyboardPlayers)) {
        keys = SDL_GetKeyboardState(NULL);
        kb = &sKeyboardPlayers[port];

        for (i = 0; i < kb->numBindings; i++) {
            if (keys[kb->bindings[i].scancode]) {
                buttons |= kb->bindings[i].button;
            }
        }

        x = axis_value(keys, kb->leftA, kb->leftB, kb->rightA, kb->rightB);
        y = axis_value(keys, kb->downA, kb->downB, kb->upA, kb->upB);

        magnitude = (x != 0 && y != 0) ? STICK_DIAG : STICK_MAX;
        x *= magnitude;
        y *= magnitude;
    }

    pads_refresh();
    pad = sPads[port];
    if (pad != NULL) {
        int px, py;

        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) buttons |= BTN_A;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) buttons |= BTN_B;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) buttons |= BTN_START;

        // Fire on the left trigger, hop / powerslide on the right, matching the
        // DC pad's mapping. The shoulder buttons double up on both.
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > PAD_TRIG_THRESHOLD ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
            buttons |= BTN_Z;
        }
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > PAD_TRIG_THRESHOLD ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            buttons |= BTN_R;
        }

        // No C-cluster on a modern pad: camera goes on the remaining face
        // buttons, and X doubles as the N64 L (debug-only in practice).
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) buttons |= BTN_CUP;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) buttons |= BTN_CDOWN | BTN_L;

        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons |= BTN_UP;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons |= BTN_DOWN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons |= BTN_LEFT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= BTN_RIGHT;

        // The stick only wins over the keyboard when actually deflected, so a
        // connected-but-idle pad does not pin ports 0 and 1 to centre.
        px = pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        py = -pad_axis(pad, SDL_CONTROLLER_AXIS_LEFTY); // SDL reports up as negative
        if (px != 0 || py != 0) {
            x = px;
            y = py;
        }
    }

    *button = buttons;
    *stickX = (signed char) x;
    *stickY = (signed char) y;
}
