#!/usr/bin/env bash
# Build the PS2 asset pack (ps2/build/bin/SSB64.DAT) from YOUR OWN ROM.
#
#   ps2/tools/prepare_assets.sh [-j N]
#
# Requirements: baserom.us.z64 in the repo root (not distributed, never
# committed), the decomp's extraction prerequisites (see README.md: python3
# with its requirements, splat), GNU make, and the PS2Build SDK (the EE gcc
# is used to compile the asset sources). On Windows run this from WSL or
# Git Bash; stage 2 can also be run on its own with Windows Python.
#
# Stages:
#   0. verify the ROM checksum
#   1. `make extract` (splat + relocData extraction) if assets/us/ is missing
#   2. make -f ps2/tools/n64prep.mk ps2-prep   (generated sources, no IDO)
#   3. python3 ps2/tools/build_assets.py       (native compile -> SSB64.DAT)
#
# Everything generated lands in ignored directories (build/, assets/,
# relocAssets/, ps2/build/). Nothing here should ever be committed.
set -euo pipefail

cd "$(dirname "$0")/../.."
JOBS="$(nproc 2>/dev/null || echo 4)"
if [ "${1:-}" = "-j" ] && [ -n "${2:-}" ]; then
    JOBS="$2"
fi
VERSION=us
ROM=baserom.$VERSION.z64
EXPECTED_SHA1=e2929e10fccc0aa84e5776227e798abc07cedabf

PY=python3
command -v "$PY" >/dev/null 2>&1 || PY=python

if [ ! -f "$ROM" ]; then
    echo "error: $ROM not found in the repo root." >&2
    echo "       Dump your own cartridge (big-endian .z64) and place it there." >&2
    exit 1
fi
sum="$(sha1sum "$ROM" | cut -d' ' -f1)"
if [ "$sum" != "$EXPECTED_SHA1" ]; then
    echo "error: $ROM has sha1 $sum, expected $EXPECTED_SHA1 (SSB64 US 1.0, .z64 byte order)." >&2
    exit 1
fi
echo "[0/3] ROM ok ($ROM)"

if [ ! -d "assets/$VERSION/relocData" ]; then
    echo "[1/3] extracting ROM (make extract)"
    make extract VERSION=$VERSION RELOC_DATA=1
else
    echo "[1/3] extraction present (assets/$VERSION), skipping"
fi

echo "[2/3] generating sources (n64prep.mk)"
make -f ps2/tools/n64prep.mk ps2-prep VERSION=$VERSION -j"$JOBS"

echo "[3/3] building the asset pack"
"$PY" ps2/tools/build_assets.py --version $VERSION -j "$JOBS"

echo "done: ps2/build/bin/SSB64.DAT (copy it next to ssb64.elf on the boot device)"
