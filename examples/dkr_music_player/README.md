# DKR Music Player

This optional enDjinn example plays DKR's 64 non-silent sequences.  It loads
the shared `music.afb`, then one small bank-bound AFX/AFC/AFV set per song.
The game itself neither needs enDjinn nor builds this directory.

The D-pad selects, A plays or pauses, B stops, LEFT/RIGHT seek ten seconds,
and L/R page.  Regular tracks come first, ambient tracks next, and short cues
last.  The playlist reports only the AFC size as requested.

Build only when you want the player.  First make enDjinn available next to the
DKR checkout, then run:

```sh
source ../../../enDjinn/environ.sh
make
make check
make bin/dkr_music_player.cdi
```

The first build regenerates DKR's CSeq-derived shared bank, flows and AFV
sidecars with the pinned AICAflow C tools.  Launch its ELF with the staged
directory as `/pc`:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q \
  -m "$PWD/cdrom/dkr_music_player" -x "$PWD/bin/dkr_music_player.elf"
```
