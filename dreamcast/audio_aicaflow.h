#ifndef DKR_AUDIO_AICAFLOW_H
#define DKR_AUDIO_AICAFLOW_H

#include <stdint.h>

int dkr_afx_init(void);
void dkr_afx_update(void);
/* Call while the DKR audio mutex is held, before a level fills the heap. */
int dkr_afx_music_prepare(uint8_t sequence);
void dkr_afx_music_prefetch(uint8_t sequence);
void dkr_afx_music_play(uint8_t sequence);
void dkr_afx_music_stop(void);
int dkr_afx_music_playing(void);
uint8_t dkr_afx_music_current(void);
void dkr_afx_music_gain(uint8_t gain);
void dkr_afx_music_tempo(uint16_t bpm);
void dkr_afx_music_lane_mute(uint8_t lane, int muted);
void dkr_afx_music_lane_volume(uint8_t lane, uint8_t volume);
void dkr_afx_music_lane_fade(uint8_t lane, uint8_t fade);
void dkr_afx_scene_reverb(uint8_t enabled);
int dkr_afx_sfx_scene_prepare(uint16_t level);
int dkr_afx_sfx_stop_all(void);
int dkr_afx_sfx_play(uint16_t id, void *owner, uint8_t priority);
void dkr_afx_sfx_stop(void *owner);
void dkr_afx_sfx_priority(void *owner, uint8_t priority);
void dkr_afx_sfx_volume(void *owner, uint8_t gain);
void dkr_afx_sfx_pitch(void *owner, float pitch);
void dkr_afx_sfx_pan(void *owner, uint8_t pan);
void dkr_afx_sfx_fx(void *owner, uint8_t fx);

#endif
