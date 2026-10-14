#ifndef DKR_DREAMCAST_SCENE_GATE_H
#define DKR_DREAMCAST_SCENE_GATE_H

// The scene gate. It closes when a scene starts loading and opens when
// everything the scene needs is resident: level and object data, textures in
// video memory, sound packs, voice packs and song in sound RAM. While it is
// closed nothing of the scene may start: no sound, no music, and the time the
// load took is not counted as game time. It opens with the scene's first frame
// on screen.

// load_level_game() and load_level_menu() call these around the whole load.
void pc_scene_loading(int level);
void pc_scene_ready(void);

// The game calls this after each logic step. The first one after a load is
// still part of it: it spawns the scene's objects, which load what they use and
// may start the song.
void pc_scene_step_done(void);

// The frame backend calls this after it has drawn a frame. The first frame
// drawn after a load opens the gate: the scene is on screen, so its sound starts.
void pc_scene_frame_shown(void);

// Whether the current scene has finished loading.
int pc_scene_is_ready(void);

// Whether the frame just drawn opened the gate, once: its time is the load's.
int pc_scene_take_opening_frame(void);

#endif
