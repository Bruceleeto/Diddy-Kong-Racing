# DKR Music Player

Dreamcast player for all 64 non-silent Diddy Kong Racing music sequences. It
uses AICAflow's shared music-player UI and lifecycle core, with DKR supplying
one complete flow per sequence, per-sequence gain, room recipe, and VIZ1
sidecars. Brief cues (under ten seconds) are listed after the regular music;
ambient beds follow the ordinary tracks, and brief cues remain at the end.
Original order is preserved within each group.

The D-pad selects, A plays or pauses the highlighted track, B stops, LEFT and
RIGHT seek ten seconds, and the triggers page through the list. Tracks that
end advance to the next item.

Build from this project:

```sh
source ../../../enDjinn/environ.sh
make
make check
make bin/dkr_music_player.cdi
```

The first build generates DKR's 64 flows and visual sidecars.
For `dc-load-ip`:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q \
  -m "$PWD/examples/dkr_music_player/cdrom/dkr_music_player" \
  -x "$PWD/examples/dkr_music_player/bin/dkr_music_player.elf"
```
