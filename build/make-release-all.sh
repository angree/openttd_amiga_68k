#!/bin/bash
# make-release-all.sh - build the COMPLETE release set in one go.
#
# Produces, for version X.Y.Z:
#   openttd-amiga-68k-X.Y.Z.zip                       Amiga, every music tier
#   openttd-amiga-68k-X.Y.Z-no-music.lha              Amiga, no music at all
#   openttd-amiga-68k-X.Y.Z-standard-soundtrack.lha   Amiga, Title + Nowe
#   openttd-amiga-68k-X.Y.Z-extended-soundtrack.lha   Amiga, Title + Nowe + Extra
#   openttd-amittd-pc-X.Y.Z-server-no-music.zip       PC/Windows, no music
#
# WHY LHA FOR THE TIERS: lha is the native Amiga archive format and always
# stores forward-slash paths. A Windows-made zip can store BACKSLASHES, which
# are not path separators on the Amiga - that shipped v0.9.1 broken (the .lng
# files never formed a lang drawer and the game died with "No available
# language packs"). The one zip we do ship is built with 7-Zip, which writes
# forward slashes, and is verified below.
#
# Music tiers are made by swapping what lives in release/openttd-amiga-68k/music
# and repacking. The full set is restored at the end.
#
# Usage (Git Bash):  bash build/make-release-all.sh [X.Y.Z]
set -e
cd "$(dirname "$0")/.."
REPO="$(pwd)"

VER="$1"
if [ -z "$VER" ]; then
  VER=$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' native/openttd/amiga_ttd_version.h | tr -d '"')
fi
[ -n "$VER" ] || { echo "FATAL: cannot determine version"; exit 1; }
echo "=== building release $VER ==="

SRC="$REPO/release/openttd-amiga-68k"
PCSRC="$REPO/AmiTTD-PC"
WAV="$REPO/WAV"
[ -d "$SRC" ]   || { echo "FATAL: $SRC missing"; exit 1; }
[ -d "$PCSRC" ] || { echo "FATAL: $PCSRC missing"; exit 1; }

SEVENZIP="/c/Program Files/7-Zip/7z.exe"
[ -x "$SEVENZIP" ] || { echo "FATAL: need 7-Zip (never Compress-Archive: it writes backslashes)"; exit 1; }

# --- sanity: the binary and the language files must come from the same build --
# `strings` is not reliably present in Git Bash and does not handle the m68k
# hunk binary, so read the revision out of it directly.
BINVER=$(python -c "
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'1.0.5 / AmiTTD ')
print(d[i:d.find(bytes([0]), i)].decode('latin-1') if i >= 0 else '')
" "$SRC/openttd")
echo "amiga binary reports: ${BINVER:-<none>}"
echo "$BINVER" | grep -q "$VER" || { echo "FATAL: release binary is not $VER - rebuild and redeploy first"; exit 1; }

mk_lha() {   # $1 = suffix
  local out="$REPO/openttd-amiga-68k-$VER$1.lha"
  rm -f "$out"
  MSYS_NO_PATHCONV=1 wsl bash -c "sudo -n mount -t drvfs I: /mnt/i 2>/dev/null; cd /mnt/i/GITHUB/Amiga_OpenTTD/release && jlha a /mnt/i/GITHUB/Amiga_OpenTTD/$(basename "$out") openttd-amiga-68k" >/dev/null 2>&1
  [ -f "$out" ] && printf "  %-56s %s\n" "$(basename "$out")" "$(du -h "$out" | cut -f1)" || echo "  FAILED: $(basename "$out")"
}

set_music() {  # $@ = tier directory names to include (none = strip music)
  rm -rf "$SRC/music"
  mkdir -p "$SRC/music"
  for t in "$@"; do
    [ -d "$WAV/$t" ] || { echo "FATAL: $WAV/$t missing"; exit 1; }
    cp -r "$WAV/$t" "$SRC/music/$t"
  done
}

echo
echo "--- tier 1/4: no music ---"
set_music
mk_lha "-no-music"

echo "--- tier 2/4: standard soundtrack (Title + Nowe) ---"
set_music Title Nowe
mk_lha "-standard-soundtrack"

echo "--- tier 3/4: extended soundtrack (everything, same as the zip) ---"
# Deliberately the SAME content as the .zip below: Amiga users who prefer lha
# must not silently lose the Stare (legacy) set just because of the format.
# The lha therefore ends up slightly larger than the zip - lha compresses WAV
# a little worse. This matches how 0.9.8 shipped.
set_music Title Nowe Extra Stare
mk_lha "-extended-soundtrack"

echo "--- tier 4/4: everything, as a zip ---"
set_music Title Nowe Extra Stare
ZIP="$REPO/openttd-amiga-68k-$VER.zip"
rm -f "$ZIP"
cd "$REPO/release"
"$SEVENZIP" a -tzip -mx=5 "$ZIP" openttd-amiga-68k >/dev/null
cd "$REPO"
# The whole point: verify no entry uses a backslash. NOTE: `7z l` DISPLAYS
# paths with backslashes on Windows regardless of what is stored, so it
# reports false positives - read the real entry names out of the zip.
BACKS=$(python -c "
import zipfile, sys
z = zipfile.ZipFile(sys.argv[1])
print(sum(1 for n in z.namelist() if chr(92) in n))
" "$ZIP")
echo "  entries with a backslash: $BACKS (must be 0)"
[ "$BACKS" = "0" ] || { echo "FATAL: backslash paths - archive is broken for Amiga"; exit 1; }
printf "  %-56s %s\n" "$(basename "$ZIP")" "$(du -h "$ZIP" | cut -f1)"

echo
echo "--- PC / Windows ---"
PCZIP="$REPO/openttd-amittd-pc-$VER-server-no-music.zip"
rm -f "$PCZIP"
STAGE="$REPO/.pcstage/openttd-amittd-pc-$VER"
rm -rf "$REPO/.pcstage"; mkdir -p "$STAGE"
cp -r "$PCSRC"/* "$STAGE"/
rm -f "$STAGE"/*.log "$STAGE"/openttd.cfg    # ship a clean, server-sane config
cp "$REPO/build/release-pc-openttd.cfg" "$STAGE/openttd.cfg" 2>/dev/null || true
cd "$REPO/.pcstage"
"$SEVENZIP" a -tzip -mx=5 "$PCZIP" "openttd-amittd-pc-$VER" >/dev/null
cd "$REPO"; rm -rf "$REPO/.pcstage"
printf "  %-56s %s\n" "$(basename "$PCZIP")" "$(du -h "$PCZIP" | cut -f1)"

echo
echo "=== done - artefacts in $REPO ==="
ls -la "$REPO"/openttd-amiga-68k-$VER*.lha "$REPO"/openttd-amiga-68k-$VER.zip "$REPO"/openttd-amittd-pc-$VER-server-no-music.zip 2>/dev/null
echo
echo "NOTE: release/openttd-amiga-68k/music now holds the FULL set again."
