#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H

// Host input layer (linux/input.c) — SDL2 keyboard.
//
// Its own translation unit for the same reason as gfx.c: SDL's headers collide
// with the N64 ones, so the libultra controller stubs (linux/reimpl.c) speak
// plain C types and read the pad through this interface.

// Fills in the state of controller port 0 in N64 form: `button` is a mask of
// the CONT_* bits from include/PR/os_cont.h, and the stick axes are the N64's
// signed range (roughly -80..80 at full deflection).
//
// Safe to call before the window exists — it reports a neutral pad until then.
void input_host_read(unsigned short *button, signed char *stickX, signed char *stickY);

#endif
