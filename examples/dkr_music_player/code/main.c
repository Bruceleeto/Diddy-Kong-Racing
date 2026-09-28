#define PLAYER_TITLE "DIDDY KONG RACING MUSIC"
#define PLAYER_MODE_NAME "DKR Music"
#define PLAYER_EXPECTED_SONGS 64
#define PLAYER_SONG_ROWS 21
#define PLAYER_SELFTEST_CASES {0}
#define PLAYER_SELFTEST_EXIT_SONG 0
#define PLAYER_SONG_GAIN(index) songs[(index)].gain
#define PLAYER_SONG_PROFILE(index) (songs[(index)].dsp ? "DKR room" : "dry")
#define PLAYER_VISUAL_FILE(index) songs[(index)].visual
#define PLAYER_SHARED_BANK_FILE "music.afb"
#define PLAYER_LIST_BYTES(index) songs[(index)].control_bytes
#define PLAYER_LIST_HEADING "AFC KiB"
#define PLAYER_ROOM_PROGRAM(program) afx_dsp_program_room((program), 20480, 12288, false, 128, false)
#include "../../../third_party/aicaflow/examples/music_player/player.c"
