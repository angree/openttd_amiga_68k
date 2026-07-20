#!/bin/bash
# ============================================================================
# OpenTTD 1.0.5 cross-build for m68k-amigaos (classic Amiga 68040)
# Phase 5a: prove the codebase COMPILES and LINKS into a 68k Hunk executable
#           using null/dedicated drivers (no native video/sound yet).
#
# Run inside WSL Ubuntu 22.04 (native gcc for host build-tools; bebbo
# amiga-gcc GCC 6.5.0b cross-compiler at /opt/amiga).
#
#   wsl bash -lc '/mnt/i/GITHUB/Amiga_OpenTTD/build/build-openttd.sh'
#
# Toolchain: see BUILD-NOTES.md. The cross-gcc MUST be rebuilt with host
# CFLAGS=-O1 (not the default -Os) or it miscompiles cc1/cc1plus on WSL1.
# ============================================================================
set -e

export PATH=/opt/amiga/bin:$PATH

# OpenTTD source lives in WSL native fs (drvfs /mnt is too slow/flaky to build on)
SRC=/home/angree/build/openttd-1.0.5

cd "$SRC"

# Clean any previous configuration
make distclean 2>/dev/null || true

# ---------------------------------------------------------------------------
# Cross-configure.
#  --enable-dedicated  : mandatory. configure refuses a null-video non-dedicated
#                        build ("no video driver development files found"); this
#                        selects ONLY dedicated_v + null_v / null_s / null_m and
#                        excludes SDL/Allegro/Cocoa/Win32. Requires network on.
#  --os=UNIX           : generic unix-like (adds -lpthread -lc -lstdc++, all
#                        present in the bebbo toolchain). Amiga-specific source
#                        activates via the compiler's built-in __AMIGA__ macro.
#  --endian=BE         : 68k is big-endian; forced because we cross-compile and
#                        configure cannot run a target test binary.
#  --without-threads   : forces thread_none.cpp (single-task; no pthread/morphos).
#  cross tools via --cc-host/--cxx-host/--strip; native host tools (strgen,
#  endian_check, settingsgen) via --cc-build/--cxx-build.
# ---------------------------------------------------------------------------
./configure \
  --host=m68k-amigaos \
  --cc-build=gcc --cxx-build=g++ \
  --cc-host=m68k-amigaos-gcc --cxx-host=m68k-amigaos-g++ \
  --strip=m68k-amigaos-strip \
  --os=UNIX \
  --endian=BE \
  --enable-dedicated \
  --without-threads \
  --disable-strip \
  --without-zlib --without-png --without-freetype --without-fontconfig \
  --without-icu --without-liblzma --without-lzo2 \
  --without-sdl --without-allegro --without-iconv \
  CFLAGS="-m68040 -noixemul -DUNIX" \
  CXXFLAGS="-m68040 -noixemul -std=gnu++98 -DUNIX" \
  LDFLAGS="-noixemul -m68040"

# Build (host tools run natively; cross tools emit 68k objects)
make VERBOSE=1

echo "=== result ==="
ls -la "$SRC"/objs/*/openttd* 2>/dev/null || ls -la "$SRC"/bin/openttd 2>/dev/null || true
