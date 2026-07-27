#!/bin/bash
# Post-configure fixups for a PC objs/release/Makefile.
# $1 = tree root.  Safe to run repeatedly (idempotent).
set -e
MF="$1/objs/release/Makefile"
[ -f "$MF" ] || { echo "no $MF"; exit 1; }
# mingw: configure fails to parse "10-win32" as a gcc version, so it drops these.
grep -q -- "-fno-strict-aliasing" <(grep "^CFLAGS  " "$MF") || \
  sed -i "s|^\(CFLAGS *=.*\)$|\1 -fno-strict-aliasing -fno-strict-overflow|" "$MF"
grep -q -- "-std=gnu++98" <(grep "^CXXFLAGS  " "$MF") || \
  sed -i "s|^\(CXXFLAGS *=.*\)$|\1 -std=gnu++98|" "$MF"
# gcc >= 4.7 rejects -mno-cygwin outright.
sed -i "s| -mno-cygwin||g" "$MF"

# Same forced -O0 set as the Amiga build (build-with-ice-retry.sh). On the PC
# side window.cpp is the proven one: at -O2 x86-64 gcc 11 the client SEGVs in
# Window::ReInit during CheckForMissingGlyphsInLoadedLanguagePack at startup.
# The other four are kept in step with the Amiga rule as cheap insurance.
grep -q "^# forced-O0" "$MF" || cat >> "$MF" <<"EOM"

# forced-O0 (see CLAUDE.md "BUILD RULES THAT MUST NOT BE LOST")
window.o cargotype.o order_cmd.o order_gui.o settings_gui.o: CFLAGS += -O0
EOM
