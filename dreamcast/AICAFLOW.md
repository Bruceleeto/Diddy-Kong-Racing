# DKR AICAFLOW loader

The music sample bank and core SFX bank stay resident. Vehicle samples stay loaded
while scenes need vehicles. A scene-local bank is preloaded when it fits with
64 KiB left for fallback; otherwise sounds load individually on demand. That
64 KiB check is a preload decision, not an allocator reservation. If concurrent
music loading consumes the measured space, a failed local-bank allocation also
falls back to individual sounds.

The generated pack contains 51 resident sounds, 24 vehicle sounds, 49 nonempty
local banks for 65 scene choices, and 784 independently loadable fallback banks.
Scene extraction reads audio objects, audio lines and animation sound IDs; the
frontend includes the intro plane and children explicitly. Runtime requests not
covered by a preloaded bank use the same fallback path.

Music and SFX have separate loader threads. File reads and DMA happen outside the
game audio mutex; completed banks are published under it. Sixteen fallback slots
cache sounds. Active instances and pending requests protect their banks from
eviction. A full load queue returns BUSY without consuming voice retries. A scene
generation discards loads completed after their scene ended.

Scene teardown cancels pending and delayed sounds, stops active SFX, and waits for
recycling before releasing banks. Instance slots are not reused before recycling.
Under memory pressure the loader releases idle fallback banks and speculative
music controls, preserving resident music samples and a song requested for
playback. A stopped predecessor yields space to replacement music. A later sound
trigger may retry a previous allocation failure; file/format errors remain errors.

Short music flows stay ready for race jingles. Loading waits are excluded from
frame timing so attract demos do not accelerate to catch up afterward. Asset-file
reads use at most 32 KiB per `/pc` transaction, directly into the destination;
this does not add a staging buffer or remove the original N64 audio bytes still
present in SH4's assets.bin. Runtime CRC is not enabled.

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
python3 -m pip install mido
```

DKR uses its own KOS build, not enDJinn. The shared environment script supplies
the toolchain and Dreamcast helpers:

```sh
source ../enDJinn/environ.sh
make -f Makefile.dc -j8
ensure_dctool_ready && dc-tool-ip -f -t "$DCTOOL_HOST:31313" -q -m "$PWD" -x "$PWD/dkracing.elf"
```

`Makefile.dc` builds the pinned AICAflow firmware and SH-4 library on demand.

To update deliberately, check out a tested AICAflow tag in the submodule and
commit the changed gitlink with its DKR validation:

```sh
git -C third_party/aicaflow fetch --tags
git -C third_party/aicaflow checkout v0.1.4-dkr
git add third_party/aicaflow
```

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
for test in dreamcast/test_aicaflow_*.py dreamcast/test_asset_reads.py; do
    python3 "$test" || exit 1
done
python3 dreamcast/build_aicaflow_sfx.py . third_party/aicaflow/tools build/dc/aicaflow --verify
python3 dreamcast/build_aicaflow_fallback.py . third_party/aicaflow/tools build/dc/aicaflow/fallback --verify
make -C third_party/aicaflow/driver/arm7
make -C third_party/aicaflow/driver/sh4
```

Lifecycle tests exercise delayed acknowledgements, cancellation, preemption,
queued bank requests and allocation failure/retry. Bank verification compares
actual IDs and file lengths with source-derived sets. Runtime music may differ
from a level header's default.
