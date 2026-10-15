#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H


// Reads the controller in `port` (0-3) as an N64 pad; returns 0 and a neutral
// pad if none is plugged in there. Safe to call before the window exists.
int input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY);

// Once a frame, before the reads: lets the controller layer take its snapshot.
void input_host_poll(void);

#endif
