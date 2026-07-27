#!/bin/bash
#
# build-both.sh - refresh the PC (x86) builds from the Amiga sources.
#
# ONE SOURCE EDIT + THIS SCRIPT = BOTH BUILDS REFRESHED.
#
#   Amiga 68k build :  bash /mnt/i/GITHUB/Amiga_OpenTTD/build/build-ai.sh
#   PC builds       :  bash /mnt/i/GITHUB/Amiga_OpenTTD/build/build-both.sh
#
# Edit the game once (in the repo's native/openttd/ mirror, or in the Amiga tree
# /home/angree/build/openttd-1.0.5/src), run build-ai.sh for the Amiga binary and
# this script for the PC ones. Both come out of the SAME sources, so the command
# table, the savegame format and _openttd_revision stay identical and a PC client
# can join an Amiga server (proven cross-endian against 31.42.176.27:3979).
#
# ------------------------------------------------------------------ SAFETY ---
# This script NEVER writes to the Amiga tree. It only reads from it.
# It NEVER runs configure/config.status/make distclean in the Amiga tree - that
# would regenerate its hand-edited objs/release/Makefile and destroy the
# -mcpu=68020 -msoft-float / custom LIBS / forced-O0 setup.
# Data flows one way only:  Amiga tree  ---->  PC trees.
# -----------------------------------------------------------------------------
#
# Targets
#   A. Linux x86-64 native  ->  /home/angree/build/ottd-pc/bin/openttd
#   B. Windows x86-64 .exe  ->  /home/angree/build/ottd-win/bin/openttd.exe
#      (mingw, stock win32_v/win32_s/win32_m drivers - no SDL needed on Windows)
#
# Usage:  build-both.sh [linux|win|both]      (default: both)

set -e

AMIGA_TREE=/home/angree/build/openttd-1.0.5
REPO=/mnt/i/GITHUB/Amiga_OpenTTD
PC_TREE=/home/angree/build/ottd-pc
WIN_TREE=/home/angree/build/ottd-win
ZLIB_MINGW=/home/angree/build/zlib-mingw
HELPERS=/home/angree/build

WHAT="${1:-both}"

# The repo copies of the helper scripts are the source of truth; install them
# next to this script so a fresh checkout is enough to build.
if [ -d "$REPO/build" ]; then
	for h in pc-source-list.sh pc-fixup-makefile.sh pc-fixes.sh pc-fixes.py; do
		[ -f "$REPO/build/$h" ] && install -m 755 "$REPO/build/$h" "$HELPERS/$h"
	done
fi

if [ ! -d "$AMIGA_TREE" ]; then echo "!! Amiga tree missing: $AMIGA_TREE"; exit 1; fi

# Files that exist ONLY in the PC trees and must never be clobbered by the sync.
#   ai_old/oldai_log_pc.c  - stdio replacement for the dos.library AI logger
#   pc_amiga_stubs.cpp     - AmigaMemProbe / AmigaMusic_* / MxSetNextSoundID stubs
PC_ONLY_EXCLUDES=(--exclude 'ai_old/oldai_log_pc.c' --exclude 'pc_amiga_stubs.cpp')

# --------------------------------------------------------------- 1. sync ----
sync_tree() {
	local DEST="$1"
	echo "=== sync src/ + source.list  ${AMIGA_TREE}  ->  ${DEST} ==="

	# The repo's native/openttd/ mirror is the real source of truth for the
	# driver + AI files; fold it into the Amiga tree's view first (read-only
	# copy into the PC tree, the Amiga tree is never touched).
	rsync -a --delete "${PC_ONLY_EXCLUDES[@]}" \
		--exclude '*.o' --exclude '*.d' \
		"$AMIGA_TREE/src/" "$DEST/src/"

	if [ -d "$REPO/native/openttd" ]; then
		rsync -a --exclude '*.o' --exclude '*.d' \
			"$REPO/native/openttd/" "$DEST/src/"
	fi

	# source.list: keep the Amiga original, generate the PC one from it.
	cp -f "$AMIGA_TREE/source.list" "$DEST/source.list.amiga"
	"$HELPERS/pc-source-list.sh" < "$DEST/source.list.amiga" > "$DEST/source.list"

	# Re-apply the PC-side portability fixes to the freshly synced sources.
	"$HELPERS/pc-fixes.sh" "$DEST"
}

# ------------------------------------------------------- 2. configure/build --
build_linux() {
	sync_tree "$PC_TREE"
	cd "$PC_TREE"
	./configure --with-sdl --with-zlib --without-liblzo2 --without-png \
		--without-freetype --without-fontconfig --without-icu --disable-strip \
		CXXFLAGS="-fpermissive" > /dev/null
	"$HELPERS/pc-fixup-makefile.sh" "$PC_TREE"
	make -j"$(nproc)"
	echo "=== LINUX OK: $PC_TREE/bin/openttd ==="
	strings "$PC_TREE/bin/openttd" | grep -m1 AmiTTD
}

build_win() {
	sync_tree "$WIN_TREE"
	cd "$WIN_TREE"
	./configure --host=x86_64-w64-mingw32 --os=MINGW \
		--without-sdl --without-allegro --without-liblzo2 --without-png \
		--without-freetype --without-fontconfig --without-icu --disable-strip \
		--with-zlib="$ZLIB_MINGW/libz.a" \
		CXXFLAGS="-fpermissive" CFLAGS="-I$ZLIB_MINGW" > /dev/null
	"$HELPERS/pc-fixup-makefile.sh" "$WIN_TREE"
	make -j"$(nproc)"
	echo "=== WINDOWS OK: $WIN_TREE/bin/openttd.exe ==="
	strings "$WIN_TREE/bin/openttd.exe" | grep -m1 AmiTTD
}

case "$WHAT" in
	linux) build_linux ;;
	win)   build_win ;;
	both)  build_linux; build_win ;;
	*)     echo "usage: $0 [linux|win|both]"; exit 1 ;;
esac

echo
echo "Reminder: the Amiga binary is NOT rebuilt by this script."
echo "Run:  bash $REPO/build/build-ai.sh"
