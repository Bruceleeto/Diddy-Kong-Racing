#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H


// Safe to call before the window exists — it reports a neutral pad until then.
void input_host_read(unsigned short *button, signed char *stickX, signed char *stickY);

#endif
