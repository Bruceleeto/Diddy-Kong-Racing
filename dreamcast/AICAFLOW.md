# DKR AICAflow integration

DKR uses the pinned `third_party/aicaflow` submodule from AICAflow main.
The game has no enDjinn dependency. The optional
[bonus music player](../bonus/dkr_music_player/README.md) uses enDjinn.

## Build and launch

Complete the ROM setup and extraction described in the
[project README](../README.md), then run from the DKR root:

```sh
git submodule update --init --recursive
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install mido
source /opt/toolchains/dc/kos/environ.sh
make -f Makefile.dc -j8
kos-tool -f -t "$DCTOOL_HOST" -m "$PWD" -x "$PWD/bin/dkracing.elf"
```

Set `DCTOOL_HOST` to the Dreamcast's address. Map the whole checkout as `/pc`:
playback reads both original game assets and `build/dc/aicaflow`.
Python 3.10+ is required; set `PYTHON=/path/to/python3` to override the build's
interpreter selection. The build compiles the SH4 library and native tools.
The included firmware needs no ARM7 toolchain.

For dc-load-ip:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q -m "$PWD" -x "$PWD/bin/dkracing.elf"
```

Keep the host server running and disable computer sleep while using `/pc`.
On macOS, `caffeinate -disu -t 1800` prevents sleep for 30 minutes.
Network latency affects on-demand asset reads.

## Disc image

```sh
make -f Makefile.dc -j8 cdi
```

Requires `mkdcdisc` on PATH. It stages assets in `cdrom/`, sets
`DKR_ASSET_MOUNT=/cd` and has enDjinn write `bin/dkracing.cdi`.
The resulting ELF uses `/cd`; run a normal build to return to `/pc`.
`DKR_ASSET_MOUNT=/pc` or `/cd` can also be selected explicitly.

## Generated audio

`afx_n64` imports CSeq/ALBank music directly: each song gets its own AFB, a
bank-bound AFX and AFC/AFV sidecars in `music_raw/`. Source IDs 2..65 are the
64 playable tracks; slot 1 is silent. The game loads one song at a time.
`afx_bank --merge` combines the `MUSIC_JINGLES` listed in `Makefile.dc` into
the always-resident `jingles.afb`. It also builds the shared `music.afb` used
by the bonus music player; the game does not load that bank.

The SFX map is [`aicaflow_tools/dkr.afsfx`](aicaflow_tools/dkr.afsfx).
It contains raw ALInstrument chain roots, grouped into core, vehicle and local
scene packs. Logical `SOUND_*` IDs resolve through
`gSoundTable[id].soundBite` in `src/audio.c`. A root includes its component
chain. Vehicle masks describe car/hovercraft/plane requirements; any nonzero
mask loads the entire vehicle bank.

The Python pack scripts read the map and generate manifests, calling
`afx_n64 --sfx` for conversion and `afx_bank --merge` for packing.
Fallback generation covers all 784 raw roots. Removing a root from a preload
pack leaves it available on demand. See the
[AFSFX specification](../third_party/aicaflow/docs/specs/afsfx.md) for grammar,
build outputs and ID rules.

The SFX importer preserves source sustain and component timing. Its offline
quality policy selects rate and PCM/ADPCM coding; merging preserves encoded
bytes. Templates retain NOTE pitch/mix for live SH4 controls.

Example generated bank file sizes:

| Bank | Bytes |
| --- | ---: |
| `core.afb` | 452,976 |
| `jingles.afb` | 323,856 |
| `vehicle.afb` | 174,672 |
| largest song (`sequence_20.afb`) | 289,968 |
| largest scene pack (`scenes/61.afb`) | 477,488 |

AICA holds bank payloads and separate flow images alongside firmware, DSP
memory and control state. File size alone is not the live allocation cost.

## Scene budget

`build_aicaflow_budget.py` plans AICA RAM at build time and writes
`music_manifest.h` and `budget.json`. Core SFX, jingles and the DSP ring are
permanent. Each scene is charged its vehicle pack, its scene pack and the
largest song, so a music change inside a level also fits. The build fails when
any scene exceeds the asset arena. To make room, move sounds out of a pack in
`dkr.afsfx` or shorten `MUSIC_JINGLES`.

## Residency and scene changes

Core SFX and jingles load once and stay resident. `level_load` then loads the
vehicle pack, the scene pack and the level's song, in that order, and returns
only when all of them are resident. The previous level's packs and idle song
are released first, so the level region is one free block. Music that keeps
playing across a scene change (menus) stays loaded; if it splits the free
space, the level region is emptied and the song reloaded and restarted.

Planned loads run on the game thread with the audio mutex held, after
dropping requested unplanned sounds and waiting for any fallback load to end. A music change outside a level load stops
the old song, releases it and loads the new one before returning.

Race voices are one pack per racer character (`voice` lines in `dkr.afsfx`),
stored as ADPCM (`afx_n64 --sfx-adpcm`) at half PCM8's size. They are not
charged to a scene. Once the racers exist, `dkr_afx_voices_prepare`
loads the packs of the characters in the race, players first, while each fits
in what the level and its song left and 64 KB stays free. A pack that does not
fit is skipped and its clips fall back to loading on demand. A later song or
unplanned sound that needs the space takes it from the least important pack
that is not playing; a scene change releases them all.

Sounds missing from every pack still load on demand through sixteen fallback
slots. There is no loader thread: the game thread calls `dkr_afx_frame()` once
a frame, holding no lock, and it loads the sounds requested since; the sound
player starts them on its next retry. Each costs a disc read in the middle of
a scene. Active instances and pending requests prevent eviction; scene
generations discard loads completed for ended scenes. Build
with `AICAFLOW_BENCHMARK=1` to log each such sound as `unplanned sound <id> in
level <n>`, then add it to a pack so it is resident when needed.

Scene teardown cancels pending/delayed sounds, stops SFX and waits for recycling
before releasing banks. File/format failures remain errors. `/pc` asset reads
use transactions of at most 32 KiB.

## Playback and DSP

DKR constructs one stereo room program at runtime with
`afx_dsp_program_room()`. Reverb settings gate its returns.

The driver does not refuse an instance because its register writes could
coincide with another's. The ARM7 runs every due event, with key-on as the
last write, and reports how many timer ticks late it was. The benchmark line
prints that as `lateness`. Voice priorities, IPC limits and DKR's one-player
eight-SFX limit constrain admission. Bound SFX rejects REBUILD.

END/STOP mutes and rapidly releases voices. Musical KEYOFF uses the authored
release envelope. Instance generations protect completed/recycled slots from
late STOP/PATCH commands.

See [SH4 integration](../third_party/aicaflow/docs/integration.md),
[asset formats](../third_party/aicaflow/docs/specs/assets.md) and
[DSP programming](../third_party/aicaflow/docs/dsp.md) for driver APIs.

## Verify

```sh
make -C third_party/aicaflow check
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow --verify
make -f Makefile.dc aicaflow-fallback-verify aicaflow-music-visuals-verify
```

AICAflow test dependencies are listed in
[Testing](../third_party/aicaflow/docs/testing.md). Pack verification checks
membership and file structure. Also test sound selection, live controls,
lifetime and scene changes in the game.

## Update the dependency

`git submodule update --init --recursive` uses the recorded commit. To select
and validate a new AICAflow main revision:

```sh
git -C third_party/aicaflow fetch origin main
git -C third_party/aicaflow checkout --detach origin/main
make -C third_party/aicaflow check
make -f Makefile.dc -j8
git add third_party/aicaflow
```

Commit the gitlink after validation. Inspect the pinned revision with
`git submodule status third_party/aicaflow`. Rebuild firmware only when changing
ARM7 code: `make -C third_party/aicaflow firmware`.
