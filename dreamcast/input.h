#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H


// Safe to call before the window exists — it reports a neutral pad until then.
// port: 0 = Puerto A (P1), 1 = Puerto B (P2), 2 = Puerto C (P3), 3 = Puerto D (P4)
void input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY);

#endif
