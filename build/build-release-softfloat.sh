#!/bin/bash
# Build the RELEASE binary: soft-float, so it runs on a 68LC040 with no FPU.
#
# Only map generation touches floating point at all - the simulation is
# integer-only by design, because OpenTTD keeps multiplayer deterministic across
# platforms - and a soft-float build generated maps with no measurable time
# penalty. So there is no reason to demand an FPU.
#
# Two traps this script exists to avoid:
#
#  1. Changing the float ABI invalidates EVERY object, including the four/five
#     that must be compiled at -O0. Building through build-with-ice-retry.sh is
#     what forces those back down; a plain make silently rebuilds them at -O1
#     and ships a broken game. Do not "simplify" this by calling make directly.
#     (The older /tmp/b.sh still carries a stale 4-file list - do not use it.)
#
#  2. amiga_gfx.o is hand-built and linked in via LIBS, so make never rebuilds
#     it. It MUST be recompiled with the same float ABI as everything else or
#     arguments are passed differently across that boundary.
#
# pow() must already be gone from tgp.cpp (build/tgp-remove-pow.py): it is the
# one libm routine proven miscompiled in soft-float here - pow(0.7,3) returned
# 0.0, which corrupted terrain generation. sin() tested correct.
set -e
export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin

TREE=/home/angree/build/openttd-1.0.5
MK="$TREE/objs/release/Makefile"
BUILD_SCRIPT="${1:-/tmp/bbuild.sh}"

cd "$TREE"

echo "== checking prerequisites =="
if ! grep -q 'amplitude \*= p' "$TREE/src/tgp.cpp"; then
    echo "REFUSING: pow() is still in tgp.cpp - run build/tgp-remove-pow.py first"
    exit 1
fi
if ! grep -q 'settings_gui.o' "$BUILD_SCRIPT"; then
    echo "REFUSING: $BUILD_SCRIPT has a stale FORCE_O0 list (no settings_gui.o)"
    exit 1
fi

echo "== switching the tree to -msoft-float =="
sed -i 's/-mhard-float/-msoft-float/g' "$MK"
grep -m1 "^CFLAGS" "$MK" | tr ' ' '\n' | grep -E "float|68040|^-O1"

# A float-ABI change invalidates everything.
find "$TREE/objs" -name '*.o' -delete
rm -f "$TREE/bin/openttd" "$TREE/objs/release/openttd"

echo "== rebuilding amiga_gfx.o with the matching ABI =="
cd "$TREE/src/video"
m68k-amigaos-gcc -O1 -m68040 -msoft-float -noixemul -c -o amiga_gfx.o amiga_gfx.c
ls -la amiga_gfx.o
cd "$TREE"

echo "== full build (forces the known-bad-codegen files to -O0) =="
bash "$BUILD_SCRIPT"

echo "== verifying the result really is soft-float =="
if grep -q -- "-mhard-float" "$MK"; then
    echo "REFUSING: the Makefile still says -mhard-float"
    exit 1
fi
ls -la "$TREE/bin/openttd"
