#include "input.h"

#include <kos.h>


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

// Full deflection on the N64 stick.
#define STICK_MAX 80

// A DC analog trigger past this (0..255) counts as a digital press.
#define TRIG_THRESHOLD 64

// maple_enum_type() indexes the controllers it finds, not the physical ports:
// a pad in port C with port B empty enumerates as controller 1. That is what we
// want — players fill N64 ports in the order their pads are plugged in.
unsigned int input_host_port_mask(void) {
    unsigned int mask = 0;
    int i;

    for (i = 0; i < MAX_PORTS; i++) {
        if (maple_enum_type(i, MAPLE_FUNC_CONTROLLER) != NULL) {
            mask |= 1u << i;
        }
    }
    return mask;
}

void input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY) {
    maple_device_t *cont;
    cont_state_t *st;
    unsigned short buttons = 0;
    int x, y;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

    if (port < 0 || port >= MAX_PORTS) {
        return;
    }

    cont = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
    if (cont == NULL) {
        return; // no pad plugged in — report neutral
    }
    st = (cont_state_t *) maple_dev_status(cont);
    if (st == NULL) {
        return;
    }

    // Face buttons and start.
    if (st->buttons & CONT_A) buttons |= BTN_A;         // accelerate
    if (st->buttons & CONT_B) buttons |= BTN_B;         // brake / reverse
    if (st->buttons & CONT_START) buttons |= BTN_START;

    // Triggers: N64 Z (fire) on the left, N64 R (hop / powerslide) on the right.
    if (st->ltrig > TRIG_THRESHOLD) buttons |= BTN_Z;
    if (st->rtrig > TRIG_THRESHOLD) buttons |= BTN_R;

    // The DC pad has no C-cluster; put camera on the X/Y face buttons, and let
    // X double as the N64 L (only used in a couple of debug spots).
    if (st->buttons & CONT_Y) buttons |= BTN_CUP;
    if (st->buttons & CONT_X) buttons |= BTN_CDOWN | BTN_L;

    // D-pad drives the N64 D-pad (menu navigation).
    if (st->buttons & CONT_DPAD_UP) buttons |= BTN_UP;
    if (st->buttons & CONT_DPAD_DOWN) buttons |= BTN_DOWN;
    if (st->buttons & CONT_DPAD_LEFT) buttons |= BTN_LEFT;
    if (st->buttons & CONT_DPAD_RIGHT) buttons |= BTN_RIGHT;

    // Analog stick: DC joyx/joyy are -128..127. Scale to the N64's -80..80, and
    // flip Y — the DC reports up as negative, the N64 as positive.
    x = (st->joyx * STICK_MAX) / 128;
    y = (-st->joyy * STICK_MAX) / 128;

    if (x < -STICK_MAX) x = -STICK_MAX;
    if (x > STICK_MAX) x = STICK_MAX;
    if (y < -STICK_MAX) y = -STICK_MAX;
    if (y > STICK_MAX) y = STICK_MAX;

    *button = buttons;
    *stickX = (signed char) x;
    *stickY = (signed char) y;
}
