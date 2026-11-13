#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H


// Reads the controller in `port` (0-3) as an N64 pad; returns 0 and a neutral
// pad if none is plugged in there. Safe to call before the window exists.
int input_host_read(int port, unsigned short *button, signed char *stickX, signed char *stickY);

// Once a frame, before the reads: lets the controller layer take its snapshot.
void input_host_poll(void);

// Rumble for the N64 channel `channel` (0-3), through a Puru Puru pack in the
// controller read as that channel. present: whether there is one. motor: the
// N64 Rumble Pak's on/off motor, as the game switches it. Once a frame,
// input_rumble_flush turns the game's switching into Puru Puru effects.
int input_rumble_present(int channel);
void input_rumble_motor(int channel, int on);
void input_rumble_flush(void);

#endif
