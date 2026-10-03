# DKR AICAFLOW loader

## Dependency and scope

DKR's `aicaflow` branch integrates the generic AICAflow **main** code through
the pinned `third_party/aicaflow` submodule. It does not use the old
`dkr-special-edition` branch. A gitlink records one exact commit, not a moving
branch subscription: ordinary `git submodule update --init --recursive`
checks out that recorded revision, even if upstream main later advances.

This guide describes game-specific residency and build policy. The dependency
documents the [runtime formats](../third_party/aicaflow/docs/specs/assets.md),
[SH4 lifecycle](../third_party/aicaflow/docs/integration.md),
[DSP](../third_party/aicaflow/docs/dsp.md) and
[AFSFX map](../third_party/aicaflow/docs/specs/afsfx.md). Read the last link
before changing SFX bank membership: it gives grammar, raw-ID translation,
examples, output paths and validation limits.

## Resident audio and scene changes

The music AFB and core SFX AFB stay resident. Vehicle samples stay loaded
while scenes need vehicles. A scene-local bank is preloaded when it fits with
64 KiB left for fallback; otherwise sounds load individually on demand. That
64 KiB check is a preload decision, not an allocator reservation. If concurrent
music loading consumes the measured space, a failed local-bank allocation also
falls back to individual sounds.

DKR builds its calibrated room DSP image at runtime with AICAflow's C
`afx_dsp_program_room()` factory, then uploads that image as the active DSP
scene. Reverb settings only gate the authored stereo returns; they do not
replace or regenerate the effect while audio is running.

The generated pack contains 54 resident sounds, 24 vehicle sounds, 49 nonempty
local banks for 65 scene choices, and 784 independently loadable fallback banks.
The tracked map was derived from audio objects, audio lines and animation
sound IDs; the frontend includes the intro plane and children explicitly.
The build consumes this explicit map, not a fresh automatic scene analysis.
Runtime requests not
covered by a preloaded bank use the same fallback path.

Music and SFX have separate loader threads. File reads and DMA happen outside the
game audio mutex; completed banks are published under it. Sixteen fallback slots
cache sounds. Active instances and pending requests protect their banks from
eviction. A full load queue returns BUSY without consuming voice retries. A scene
generation discards loads completed after their scene ended.

Scene teardown cancels pending and delayed sounds, stops active SFX, and waits for
recycling before releasing banks. Instance slots are not reused before recycling.
Under memory pressure the loader releases idle fallback banks and an unneeded
title-demo prefetch, preserving the resident music bank and a song requested for
playback. A stopped predecessor yields space to replacement music. A later sound
trigger may retry a previous allocation failure; file/format errors remain errors.

Short music flows stay ready for race jingles. The title menu may prepare one
next-demo flow so the transition stays gap-free; it is otherwise discarded before
it competes with scene SFX. Loading waits are excluded from frame timing so
attract demos do not accelerate to catch up afterward. Asset-file reads use at
most 32 KiB per `/pc` transaction, directly into the destination; this does not
add a staging buffer or remove the original N64 audio bytes still present in
SH4's assets.bin. Runtime CRC is not enabled.

## Playback lifetime and limits

SH4 releases a bound SFX's stream-work reservation after ARM7 reports PARK. Its
channels, samples and references remain owned. Bound SFX already reject REBUILD,
so their stream cannot resume. Rebuildable flows retain their reservation.
The calibrated 171-register-write limit and separate IPC limits are unchanged.

END/STOP retirement mutes and rapidly releases owned voices; authored musical
KEYOFF retains its release envelope. STOP racing natural completion acknowledges
the matching completed generation. Late PATCH commands cannot overwrite a
completed instance or a newer generation; the pinned AICAflow runtime enforces
that protocol.

Voice priorities and finite hardware/execution capacity still apply: having a
sound loaded does not guarantee every simultaneous playback request is admitted.
The parked-work fix increased the observed attract-mode peak from four to eight
SFX (DKR's one-player voice limit). It does not promise arbitrary combinations.

## Build and launch

DKR pins its AICAflow dependency as `third_party/aicaflow`. Initialise it on
an existing checkout (a fresh clone should use `--recurse-submodules`):

```sh
git submodule update --init --recursive
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install mido
```

DKR's AICAflow build requires Python 3.10+. `Makefile.dc` selects a suitable
interpreter automatically; set `PYTHON=/path/to/python3.10-or-newer` to choose one explicitly.

DKR uses its own KOS build, not enDjinn. For the toolchain:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -f Makefile.dc -j8
```

Use the host tool installed on your machine, with the whole DKR checkout
mapped as `/pc`. Both the original game assets and `build/dc/aicaflow` are
needed; mapping only the music-player staging directory is wrong for the game.
For example, when kos-load is ready:

```sh
kos-tool -f -t "$DCTOOL_HOST" -m "$PWD" -x "$PWD/dkracing.elf"
```

For a dc-load-ip target instead (the optional sibling enDjinn environment
provides the local `ensure_dctool_ready` helper):

```sh
source ../enDjinn/environ.sh
ensure_dctool_ready && dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q -m "$PWD" -x "$PWD/dkracing.elf"
```

`Makefile.dc` uses the current pinned AICAflow `main` runtime and builds the
SH-4 library and native tools when their sources change, including after a
submodule update. Ignored binaries cannot silently keep the previous importer.
An ARM7 toolchain is only needed when modifying the
firmware. DKR music is generated by AICAflow's native `build/afx_n64` CSeq /
ALBank reader and native `build/afx_bank --merge`: each CSeq becomes a small
bank-bound AFX/AFC/AFV set, then byte-identical samples are merged into one
1 MiB-ish `music.afb`. There is no MIDI round-trip.

DKR owns only its SFX pack policy in `dreamcast/aicaflow_tools`: which sounds
are resident, vehicle- or scene-local, and its source-derived manifest. Those
pack orchestration scripts still use Python; SFX parsing, sample selection,
encoding, flow emission and AFV generation use the native C tools. OoT's
distinct experimental AudioSeq reader remains in AICAflow research tools.

To update deliberately, move the submodule to a tested AICAflow main commit and
commit the changed gitlink with its DKR validation:

```sh
git -C third_party/aicaflow fetch origin main
git -C third_party/aicaflow checkout --detach origin/main
make -C third_party/aicaflow check
make -f Makefile.dc -j8
git add third_party/aicaflow
```

Run this deliberately, not as an automatic build-time update. Commit the
gitlink only after validation. To inspect the pinned commit without changing
it, use `git submodule status third_party/aicaflow` and
`git -C third_party/aicaflow log -1 --oneline`.

Asset paths default to `/pc`. Build with `make -f Makefile.dc -j8 DKR_ASSET_MOUNT=/cd`
for a disc image, or `DKR_ASSET_MOUNT=/pc` for dc-load-ip. Changing the mount
automatically rebuilds the path users; no clean build is needed. The directory
layout beneath the mount stays the same.

Build a self-contained disc image with `make -f Makefile.dc -j8 cdi`.
This requires `mkdcdisc` on PATH (or `MKDCDISC=/path/to/mkdcdisc`), builds with
`DKR_ASSET_MOUNT=/cd`, stages game assets and runtime audio files in `cdrom/`,
and writes `dkracing.cdi`. The ELF is left configured for `/cd`; a normal build
switches it back to `/pc`. `cdrom/` and CDI images are ignored by Git.

Keep the `/pc` server alive until the Dreamcast is reset. For unattended tests,
run `caffeinate -disu -t 1800` in a separate session; it survives server replacement
and expires after 30 minutes. macOS idle sleep interrupts `/pc` and capture.
The installed dc-tool retries temporary UDP send-buffer exhaustion and handles
delayed acknowledgements; its tests are in the neighboring dcload-ip repository.
These changes do not guarantee recovery from arbitrary network failures.

## Verification

```sh
make -C third_party/aicaflow check
python3 dreamcast/build_aicaflow_sfx.py . \
  third_party/aicaflow/build/afx_n64 third_party/aicaflow/build/afx_bank \
  dreamcast/aicaflow_tools/dkr.afsfx build/dc/aicaflow --verify
make -f Makefile.dc aicaflow-fallback-verify aicaflow-music-visuals-verify
make -C third_party/aicaflow/driver/sh4
```

The checked-in `third_party/aicaflow/firmware/aicaflow.drv` lets normal DKR
builds work without an ARM7 toolchain. Rebuild it only after firmware changes:
`make -C third_party/aicaflow firmware`.

Each AFX is sample-free and binds once to exactly one AFB. AFC files are
optional SH4 seek indexes; normal game playback needs only the AFB and AFX.
Bank verification compares actual IDs and file lengths with source-derived
sets. Runtime music may differ from a level header's default.

`dreamcast/aicaflow_tools/dkr.afsfx` is the checked-in SFX residency map. It
lists the raw N64 sound IDs in the resident core/vehicle banks and in every
level-local bank, plus the per-scene vehicle masks. Edit that file when an
intentional SFX residency decision changes; the build uses AICAflow's native
`afx_n64 --sfx` and `afx_bank --merge` tools to emit the final assets.
It is **not** an input to `afx_bank` itself or a runtime file. The Python
application wrapper reads the map and calls the C tools. It does not synthesize
the sound or convert through MIDI. The
[AFSFX specification](../third_party/aicaflow/docs/specs/afsfx.md) distinguishes
the reusable grouping role from this current DKR-specific reader.

The IDs are raw, one-based ALInstrument sound-chain roots. A game's logical
`SOUND_*` enum is first resolved through `gSoundTable[id].soundBite` in
`src/audio.c`. Thus an inventory-pickup enum is not necessarily the ID to pass
to `afx_n64 --sfx`. A root can expand to multiple components/channels; its
whole chain is packed even when those component IDs are not listed separately.
The vehicle masks record car/hovercraft/plane requirements, but any nonzero
mask currently loads the **whole single vehicle bank**, not a type-specific
subset.

Fallback generation independently covers all 784 raw roots. Removing an ID
from a preload pack does not delete that sound; it moves its requests to the
on-demand path. Generated `manifest.json` and `sfx_manifest.h` are build outputs,
not alternate source maps. The generic driver's reference handling prevents
a bank from being freed while its flows/instances still retain it.

The SFX importer preserves source sustain and component timing, choosing
sample rate and PCM/ADPCM coding with its offline quality policy. Its templates carry the same pitch
and mix as each NOTE, which the game's SH4 controls use as their baseline.
Bank merging preserves those authored sample bytes and formats.

As measured on 2026-10-03, `music.afb` is
1,060,176 bytes, `core.afb` 452,976 bytes and `vehicle.afb` 174,672 bytes.
These are **file sizes**, not a promise of allocator capacity; AICA loads bank
payloads and separate flow images alongside firmware/DSP/control state.
The music set contains 64 playable controls (source IDs 2..65); source slot 1
is not a playable track. Song changes load small controls against the shared
music bank, not a newly compiled or reloaded per-song sample set.

Pack verification checks map/output structure, not whether a logical ID was
mapped to the correct sound by a human. Test sound selection, lifetime and
scene changes in the game.

With `/pc` mounting, asset reads during scene/song transitions still travel
over the host network. Wi-Fi latency can lengthen these waits even though only
a small AFX, not the resident music AFB, is loaded for a song change.

## Optional DKR music player

`bonus/dkr_music_player` is a separate enDjinn project that reuses
`third_party/aicaflow/examples/player_framework/music_player.c`. It is not a
dependency of DKR itself; only provide enDjinn when building this player. See
its [README](../bonus/dkr_music_player/README.md) for build and launch
commands.
