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

// Full deflection on the N64 stick.
#define STICK_MAX 80

// A DC analog trigger past this (0..255) counts as a digital press.
#define TRIG_THRESHOLD 64

// Stick deflection (post-scale) below this counts as idle, letting the D-pad
// take over that axis. Covers centering drift without eating real input.
#define STICK_IDLE 8

void input_host_read(unsigned short *button, signed char *stickX, signed char *stickY) {
    maple_device_t *cont;
    cont_state_t *st;
    unsigned short buttons = 0;
    int x, y;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

    cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
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

    // The game only reads the stick for steering/pitch, so the D-pad also
    // synthesizes full stick deflection on any axis the stick leaves idle.
    if (x > -STICK_IDLE && x < STICK_IDLE) {
        if (st->buttons & CONT_DPAD_LEFT) x = -STICK_MAX;
        if (st->buttons & CONT_DPAD_RIGHT) x = STICK_MAX;
    }
    if (y > -STICK_IDLE && y < STICK_IDLE) {
        if (st->buttons & CONT_DPAD_UP) y = STICK_MAX;
        if (st->buttons & CONT_DPAD_DOWN) y = -STICK_MAX;
    }

    *button = buttons;
    *stickX = (signed char) x;
    *stickY = (signed char) y;
}
