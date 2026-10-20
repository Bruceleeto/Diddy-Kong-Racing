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

#if DKR_ENJ_INPUT
#include <enDjinn/enj_ctrl.h>
#include <enDjinn/enj_state.h>

// enDjinn's controller layer (ENJ_INPUT=1): one snapshot of every port a
// frame, taken by enDjinn's loop (ENJ_LOOP=1) or by input_host_poll.
extern void enj_ctrl_init_local_devices(void);
static int sEnjInputReady;

void input_host_poll(void) {
    enj_state_t *state = enj_state_get();

    if (!sEnjInputReady) {
        sEnjInputReady = 1;
        // enj_state_startup set the controllers up unless told not to.
        if (!state->flags.started || state->flags.no_input_init) {
            enj_ctrl_init_local_devices();
        }
    }
#if !DKR_ENJ_LOOP
    enj_ctrl_map_states();
#endif
}

#define HELD(s, b) (((s)->button.b) & ENJ_BUTTON_DOWN)
#else
void input_host_poll(void) {
}
#endif

int input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY) {
    unsigned short buttons = 0;
    int x, y;
    int a, b, start, ltrig, rtrig, bx, by, up, down, left, right, joyx, joyy;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

#if DKR_ENJ_INPUT
    {
        const enj_ctrlr_state_t *st;

        if (port < 0 || port >= (int) enj_ctrl_states_length() || !sEnjInputReady) {
            return 0;
        }
        st = enj_ctrl_get_states()[port];
        if (st == NULL) {
            return 0; // no pad plugged in - report neutral
        }
        a = HELD(st, A);
        b = HELD(st, B);
        start = HELD(st, START);
        bx = HELD(st, X);
        by = HELD(st, Y);
        up = HELD(st, UP);
        down = HELD(st, DOWN);
        left = HELD(st, LEFT);
        right = HELD(st, RIGHT);
        ltrig = st->ltrigger;
        rtrig = st->rtrigger;
        joyx = st->joyx;
        joyy = st->joyy;
    }
#else
    {
        maple_device_t *cont;
        cont_state_t *st;

        cont = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
        if (cont == NULL) {
            return 0; // no pad plugged in - report neutral
        }
        st = (cont_state_t *) maple_dev_status(cont);
        if (st == NULL) {
            return 0;
        }
        a = (st->buttons & CONT_A) != 0;
        b = (st->buttons & CONT_B) != 0;
        start = (st->buttons & CONT_START) != 0;
        bx = (st->buttons & CONT_X) != 0;
        by = (st->buttons & CONT_Y) != 0;
        up = (st->buttons & CONT_DPAD_UP) != 0;
        down = (st->buttons & CONT_DPAD_DOWN) != 0;
        left = (st->buttons & CONT_DPAD_LEFT) != 0;
        right = (st->buttons & CONT_DPAD_RIGHT) != 0;
        ltrig = st->ltrig;
        rtrig = st->rtrig;
        joyx = st->joyx;
        joyy = st->joyy;
    }
#endif

    // Face buttons and start.
    if (a) buttons |= BTN_A;         // accelerate
    if (b) buttons |= BTN_B;         // brake / reverse
    if (start) buttons |= BTN_START;

    // Triggers: N64 Z (fire) on the left, N64 R (hop / powerslide) on the right.
    if (ltrig > TRIG_THRESHOLD) buttons |= BTN_Z;
    if (rtrig > TRIG_THRESHOLD) buttons |= BTN_R;

    // The DC pad has no C-cluster; put camera on the X/Y face buttons, and let
    // X double as the N64 L (only used in a couple of debug spots).
    if (by) buttons |= BTN_CUP;
    if (bx) buttons |= BTN_CDOWN | BTN_L;

    // D-pad drives the N64 D-pad (menu navigation).
    if (up) buttons |= BTN_UP;
    if (down) buttons |= BTN_DOWN;
    if (left) buttons |= BTN_LEFT;
    if (right) buttons |= BTN_RIGHT;

    // Analog stick: DC joyx/joyy are -128..127. Scale to the N64's -80..80, and
    // flip Y — the DC reports up as negative, the N64 as positive.
    x = (joyx * STICK_MAX) / 128;
    y = (-joyy * STICK_MAX) / 128;

    if (x < -STICK_MAX) x = -STICK_MAX;
    if (x > STICK_MAX) x = STICK_MAX;
    if (y < -STICK_MAX) y = -STICK_MAX;
    if (y > STICK_MAX) y = STICK_MAX;

    // The game only reads the stick for steering/pitch, so the D-pad also
    // synthesizes full stick deflection on any axis the stick leaves idle.
    if (x > -STICK_IDLE && x < STICK_IDLE) {
        if (left) x = -STICK_MAX;
        if (right) x = STICK_MAX;
    }
    if (y > -STICK_IDLE && y < STICK_IDLE) {
        if (up) y = STICK_MAX;
        if (down) y = -STICK_MAX;
    }

    *button = buttons;
    *stickX = (signed char) x;
    *stickY = (signed char) y;
    return 1;
}
