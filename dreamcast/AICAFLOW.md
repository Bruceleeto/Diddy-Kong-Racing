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
kos-tool -f -t "$DCTOOL_HOST" -m "$PWD" -x "$PWD/dkracing.elf"
```

Set `DCTOOL_HOST` to the Dreamcast's address. Map the whole checkout as `/pc`:
playback reads both original game assets and `build/dc/aicaflow`.
Python 3.10+ is required; set `PYTHON=/path/to/python3` to override the build's
interpreter selection. The build compiles the SH4 library and native tools.
The included firmware needs no ARM7 toolchain.

For dc-load-ip:

```sh
dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q -m "$PWD" -x "$PWD/dkracing.elf"
```

Keep the host server running and disable computer sleep while using `/pc`.
On macOS, `caffeinate -disu -t 1800` prevents sleep for 30 minutes.
Network latency affects on-demand asset reads.

## Disc image

```sh
make -f Makefile.dc -j8 cdi
```

Requires `mkdcdisc` on PATH, or `MKDCDISC=/path/to/mkdcdisc`. It stages assets
in `cdrom/`, sets `DKR_ASSET_MOUNT=/cd` and writes `dkracing.cdi`.
The resulting ELF uses `/cd`; run a normal build to return to `/pc`.
`DKR_ASSET_MOUNT=/pc` or `/cd` can also be selected explicitly.

## Generated audio

`afx_n64` imports CSeq/ALBank music directly. `afx_bank --merge` combines
encoded samples into one shared `music.afb`; each song has a bank-bound AFX
and AFC/AFV sidecars. Source IDs 2..65 are the 64 playable tracks; slot 1 is
silent. Normal game playback needs AFB/AFX; AFC is optional seeking data.

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
| `music.afb` | 1,060,176 |
| `core.afb` | 452,976 |
| `vehicle.afb` | 174,672 |

AICA holds bank payloads and separate flow images alongside firmware, DSP
memory and control state. File size alone is not the live allocation cost.

## Residency and scene changes

Music and core SFX stay resident. Vehicle stays loaded while needed. Local
scene packs are preloaded when they leave 64 KiB headroom for fallback;
otherwise sounds load on demand. This threshold is not an allocator reservation.
The map contains 54 core roots, 24 vehicle roots and 49 local packs for 65 scenes.

Music and SFX use separate loader threads. Reads and DMA run outside the audio
mutex; completed banks are published under it. Sixteen fallback slots cache
sounds. Active instances and pending requests prevent eviction. A full queue
returns BUSY; scene generations discard loads completed for ended scenes.

Scene teardown cancels pending/delayed sounds, stops SFX and waits for recycling
before releasing banks. Under memory pressure, idle fallback banks and unused
title-demo prefetch are released. Replacement music can reclaim its stopped
predecessor. File/format failures remain errors.

Race jingles stay prepared. The title menu can prefetch one next-demo flow.
Loading waits are excluded from attract-demo timing. `/pc` asset reads use
transactions of at most 32 KiB.

## Playback and DSP

DKR constructs one stereo room program at runtime with
`afx_dsp_program_room()`. Reverb settings gate its returns.

PARK releases a bound SFX's stream-work reservation while retaining voices,
samples and references. Bound SFX rejects REBUILD. Rebuildable flows retain
their reservation. The per-tick register-write limit is 171; voice priorities,
IPC limits and DKR's one-player eight-SFX limit also constrain admission.

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
