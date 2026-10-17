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

void input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY) {
    maple_device_t *cont;
    cont_state_t *st;
    unsigned short buttons = 0;
    int x, y;

    *button = 0;
    *stickX = 0;
    *stickY = 0;

    cont = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
    if (cont == NULL) {
        return;
    }
    st = (cont_state_t *) maple_dev_status(cont);
    if (st == NULL) {
        return;
    }

    // 🟢 1. Botones frontales (A: Acelerar | B y X: Freno doble | Y: Cámara)
    if (st->buttons & CONT_A) buttons |= BTN_A;     // A: Acelerar
    if (st->buttons & CONT_B) buttons |= BTN_B;     // B: Frenar / Reversa
    if (st->buttons & CONT_X) buttons |= BTN_B;     // X: Frenar / Reversa
    if (st->buttons & CONT_Y) buttons |= BTN_CUP;   // Y: Zoom de cámara

    // 🟢 2. Gatillos (L: Armas y corneta original | R: Derrape y salto)
    if (st->ltrig > TRIG_THRESHOLD) buttons |= BTN_Z; // L: Disparar armas / Corneta
    if (st->rtrig > TRIG_THRESHOLD) buttons |= BTN_R; // R: Derrape / Salto

    // 🟢 3. Start
    if (st->buttons & CONT_START) buttons |= BTN_START; // Pausa

    // 🟢 4. Cruceta (D-Pad)
    if (st->buttons & CONT_DPAD_UP)    buttons |= BTN_CUP;    // ⬆️ Arriba: Zoom de cámara (igual que Y)
    if (st->buttons & CONT_DPAD_RIGHT) buttons |= BTN_CRIGHT; // ➡️ Derecha: TT CAM / Mapa / Radar
    if (st->buttons & CONT_DPAD_LEFT)  buttons |= BTN_CDOWN;  // ⬅️ Izquierda: Velocímetro / Info carrera
    // ⬇️ Abajo queda VACÍO (no hace nada)

    // 🟢 5. Palanca Analógica (Conducción limpia)
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