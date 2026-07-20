#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H

// Host input layer (linux/input.c, dreamcast/input.c) — SDL2 keyboard and game
// controllers on PC, maple pads on DC.
//
// Its own translation unit for the same reason as gfx.c: SDL's headers collide
// with the N64 ones, so the libultra controller stubs (reimpl.c) speak plain C
// types and read the pads through this interface.

// Bitmask of which of the four controller ports currently have a device on them
// (bit N = port N). The game only lets a second player join at character select
// if a second port actually reports input, and that join is what gates
// two-player adventure (JOINTVENTURE) and split screen.
unsigned int input_host_port_mask(void);

// Fills in the state of controller port `port` (0..3) in N64 form: `button` is
// a mask of the CONT_* bits from include/PR/os_cont.h, and the stick axes are
// the N64's signed range (roughly -80..80 at full deflection).
//
// Safe to call before the window exists, and for an empty port — it reports a
// neutral pad in both cases.
void input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY);

#endif
