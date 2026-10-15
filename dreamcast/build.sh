#!/usr/bin/env bash
# Builds the Dreamcast port from a clean clone and your own ROM.
#
# usage: dreamcast/build.sh [--cdi] [make arguments for Makefile.dc ...]
#
# Put a Diddy Kong Racing US 1.0 ROM (.z64) in baseroms/ first. Everything
# else (assets, PVR textures, scene bundles, AICAflow sound and music) is
# generated from it; nothing from the ROM is in this repository.
#
# Needs a KallistiOS toolchain with its environment sourced
# (source /opt/toolchains/dc/kos/environ.sh), KOS's pvrtex utility built, the
# sh4zam addon installed, Python 3.10 or newer, and mkdcdisc for --cdi.
set -euo pipefail

cd "$(dirname "$0")/.." >/dev/null

cdi=0
if [[ ${1:-} == --cdi ]]; then
    cdi=1
    shift
fi

fail() {
    echo "build.sh: $*" >&2
    exit 1
}

# --- What has to be there before anything runs. ---
[[ -n ${KOS_BASE:-} ]] || fail "KOS_BASE is not set; source your KallistiOS environ.sh first"
command -v sh-elf-gcc >/dev/null || command -v "${KOS_CC:-kos-cc}" >/dev/null ||
    fail "the sh-elf toolchain is not on PATH; source your KallistiOS environ.sh first"
pvrtex=${PVRTEX:-$KOS_BASE/utils/pvrtex/pvrtex}
[[ -x $pvrtex ]] || fail "no pvrtex at $pvrtex; build it with: make -C $KOS_BASE/utils/pvrtex"
[[ -f $KOS_BASE/addons/lib/dreamcast/libsh4zam.a ]] ||
    fail "the sh4zam addon is not installed in $KOS_BASE/addons (see https://github.com/gyrovorbis/sh4zam)"
python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null ||
    fail "Python 3.10 or newer is needed as python3"
if [[ $cdi == 1 ]]; then
    command -v mkdcdisc >/dev/null || fail "mkdcdisc is needed on PATH for --cdi"
fi
shopt -s nullglob
roms=(baseroms/*.z64)
shopt -u nullglob
(( ${#roms[@]} > 0 )) || fail "put your Diddy Kong Racing US 1.0 ROM (.z64) in baseroms/"

jobs=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# --- The build. ---
echo "== submodules"
git submodule update --init --recursive
# AICAflow pins AICAforge with update = none; Makefile.dc fetches it when needed.

echo "== python environment and tools (make setup)"
make setup

echo "== extracting the ROM's assets (make extract)"
make extract

echo "== packing the assets (make build_assets)"
make build_assets

echo "== the Dreamcast build (make -f Makefile.dc)"
make -f Makefile.dc -j"$jobs" "$@"

if [[ $cdi == 1 ]]; then
    echo "== the disc image (make -f Makefile.dc cdi)"
    make -f Makefile.dc cdi "$@"
fi
echo "== done: bin/dkracing.elf$([[ $cdi == 1 ]] && echo ' and bin/dkracing.cdi')"
