#include "input.h"

#include <kos.h>
#include <dc/maple/purupuru.h>
#include <enDjinn/enj_rumble.h>
#include <enDjinn/enj_state.h>


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
// frame, taken by enDjinn's loop.
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

// ---------------------------------------------------------------------------
// Rumble. The N64 Rumble Pak has one motor, on or off; the game sets a
// strength by switching it on for a share of the frames (src/save_data.c,
// rumble_update). Each channel keeps which of its last four frames had the
// motor on, and that share becomes the Puru Puru's strength, sent only when it
// moves by two steps or the motor starts or stops, so maple sees changes only.
// A start goes out at full strength at once: built up from the share, a hit
// reached full strength only after four to eight game frames and felt late.
// ---------------------------------------------------------------------------
#define RUMBLE_FREQ 26 // the Puru Puru's vibration frequency setting
#define RUMBLE_MAX 4   // the strongest fpow sent; 7 shook too hard for the game's hits

extern void enj_rumble_init_local_devices(void);
extern void enj_rumble_update(void);

static int sRumbleReady;
static unsigned char sRumbleOn[4];      // the motor as the game last set it
static unsigned char sRumbleHistory[4]; // one bit per frame, the newest lowest
static unsigned char sRumbleSent[4];    // the strength sent last, 0 for stopped

// The maple port that the game reads as `channel`, or -1.
static int rumble_port(int channel) {
    if (channel < 0 || channel >= 4) {
        return -1;
    }
#if DKR_ENJ_INPUT
    return channel; // enDjinn's controller states are by port
#else
    {
        // The plain path reads the channel's controller by enumeration order.
        maple_device_t *cont = maple_enum_type(channel, MAPLE_FUNC_CONTROLLER);

        return cont != NULL ? cont->port : -1;
    }
#endif
}

static void rumble_init(void) {
    enj_state_t *state = enj_state_get();

    sRumbleReady = 1;
    // enj_state_startup scans for packs unless told to leave input alone.
    if (!state->flags.started || state->flags.no_input_init) {
        enj_rumble_init_local_devices();
    }
}

int input_rumble_present(int channel) {
    int port;

    if (!sRumbleReady) {
        rumble_init();
    }
    port = rumble_port(channel);
    return port >= 0 && enj_rumble_states_get()[port] != NULL;
}

void input_rumble_motor(int channel, int on) {
    if (channel >= 0 && channel < 4) {
        sRumbleOn[channel] = on != 0;
    }
}

void input_rumble_flush(void) {
    int i;

    if (!sRumbleReady) {
        return; // the game never looked for a pack
    }
    for (i = 0; i < 4; i++) {
        int frames, strength, port;

        sRumbleHistory[i] = (unsigned char) (((sRumbleHistory[i] << 1) | sRumbleOn[i]) & 0xF);
        frames = __builtin_popcount(sRumbleHistory[i]);
        // 1..4 frames on of 4 -> fpow up to RUMBLE_MAX, none -> stopped.
        strength = (frames * RUMBLE_MAX + 2) / 4;
        if (sRumbleOn[i] && sRumbleSent[i] == 0) {
            strength = RUMBLE_MAX;
        }
        if (strength == sRumbleSent[i] ||
            (strength != 0 && sRumbleSent[i] != 0 && strength - sRumbleSent[i] < 2 &&
             sRumbleSent[i] - strength < 2)) {
            continue;
        }
        port = rumble_port(i);
        if (port < 0) {
            continue;
        }
        if (strength == 0) {
            purupuru_effect_t stop = { .motor = 1 }; // a raw 0 would not be sent
            enj_rumble_effect_set(port, stop);
        } else {
            purupuru_effect_t effect = { .cont = 1, .motor = 1, .fpow = strength, .freq = RUMBLE_FREQ };
            enj_rumble_effect_set(port, effect);
        }
        sRumbleSent[i] = (unsigned char) strength;
    }
}
