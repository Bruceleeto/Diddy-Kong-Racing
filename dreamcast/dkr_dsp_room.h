#ifndef DKR_DSP_ROOM_H
#define DKR_DSP_ROOM_H

#include <aicaflow/dsp.h>

/* Build DKR's AL_FX_CUSTOM room at runtime.  The source network runs at
   22.05 kHz; this AICA program uses its exact topology at 44.1 kHz. */
int dkr_dsp_program_n64_room(afx_dsp_program_t *program);

/* Measurement-only form of the same room. sections is 1..5 in the original
   N64 order, so the impulse bench can isolate each feedback section. */
int dkr_dsp_program_n64_room_sections(afx_dsp_program_t *program, uint8_t sections);

#endif
