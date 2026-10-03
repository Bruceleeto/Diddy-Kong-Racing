# DKR Music Player

This optional enDjinn bonus player plays DKR's 64 non-silent sequences. It loads
the shared `music.afb`, then one small bank-bound AFX/AFC/AFV set per song.
The game itself neither needs enDjinn nor builds this directory.

The D-pad selects, A plays or pauses, B stops, LEFT/RIGHT seek ten seconds,
and L/R page.  Regular tracks come first, ambient tracks next, and short cues
last. The playlist reports only the AFC file size in KiB (rounded up).
AFC contains SH4-only seek checkpoints; it is not uploaded to AICA and the
displayed size excludes the AFX control stream and shared AFB sample bank.
The AFC header is included in the file size; SH4 retains its checkpoint payload
after validation. Seeking sends reconstructed voice state, not the AFC table,
to AICA. The footer's `AICA` figure is a separate flow-image figure; with this
shared bank, `S 0K/0` means no per-song sample allocation, not silent music or
a missing `music.afb`. The shared bank is still resident.

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

For kos-load/kos-tool instead:

```sh
kos-tool -f -t "$DCTOOL_HOST" \
  -m "$PWD/cdrom/dkr_music_player" -x "$PWD/bin/dkr_music_player.elf"
```

Keep the host server/computer awake while using `/pc`; the player reads songs
and visualization data after ELF startup. START+A+B+X+Y exits through the
enDjinn loop. For Flycast or an offline recording, use the generated CDI.

`prepare.py` stages existing authored files and builds `include/songs.h` with
actual AFC byte sizes and source-derived titles/gain/reverb metadata; it is
not another music compiler. AFX/AFC reads are synchronous. AFV reads use the
shared player loop; song changes reuse the resident AFB. The game alone uses
the [AFSFX grouping map](../../third_party/aicaflow/docs/specs/afsfx.md); this
music-only player does not need or interpret it.
