# DKR music player

An optional enDjinn player for DKR's 64 playable sequences. It keeps one
shared `music.afb` resident and loads each song's AFX/AFC/AFV sidecars.
The game itself does not depend on this player or enDjinn.

## Build and launch

Place enDjinn next to the DKR checkout. From the DKR root:

```sh
cd bonus/dkr_music_player
source ../../../enDjinn/environ.sh
make
make check
kos-tool -f -t "$DCTOOL_HOST" \
  -m "$PWD/cdrom/dkr_music_player" -x "$PWD/bin/dkr_music_player.elf"
```

For dc-load-ip, use the same mapped directory:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q \
  -m "$PWD/cdrom/dkr_music_player" -x "$PWD/bin/dkr_music_player.elf"
```

Keep the host server and computer awake during `/pc` playback. For Flycast or
offline playback:

```sh
make bin/dkr_music_player.cdi
```

## Controls and display

D-pad selects, A plays/pauses, B stops, left/right seeks ten seconds, and L/R
pages. START+A+B+X+Y exits. Regular songs precede ambient tracks and short cues.

The playlist shows AFC file size rounded up to KiB, including its header.
AFC seek data stays on SH4. The footer's `AICA` value is the flow-image cost;
`S 0K/0` indicates no per-song sample allocation because the shared bank is
resident. Seeking sends reconstructed voice state to AICA, not the AFC table.

## Assets

The build generates music with AICAflow's C tools. `prepare.py` stages assets
and writes `include/songs.h` with titles, AFC sizes and gain/reverb metadata.
AFX/AFC reads are synchronous; AFV reads are stepped through the shared
[enDjinn player framework](../../third_party/aicaflow/examples/player_framework/README.md).
