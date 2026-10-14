#ifndef DKR_DREAMCAST_SCENE_GATE_H
#define DKR_DREAMCAST_SCENE_GATE_H

// The scene gate. It closes when a scene starts loading and opens when
// everything the scene needs is resident: level and object data, textures in
// video memory, sound packs, voice packs and song in sound RAM. While it is
// closed nothing of the scene may start: no sound, no music, and the time the
// load took is not counted as game time.

// load_level_game() and load_level_menu() call these around the whole load.
void pc_scene_loading(void);
void pc_scene_ready(void);

// Whether the current scene has finished loading.
int pc_scene_is_ready(void);

#endif
