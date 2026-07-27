#!/bin/bash
# make-release-zip.sh - pack the Amiga release archive THE ONE CORRECT WAY.
#
# WHY THIS SCRIPT EXISTS (v0.9.1 shipped broken because of this):
#   PowerShell `Compress-Archive` writes zip entries with BACKSLASH path
#   separators ("openttd-amiga-68k\lang\english.lng"). Backslash is NOT a path
#   separator on the Amiga (or any POSIX system), so on extraction the .lng files
#   do NOT land in a `lang` drawer - they become flat files with literal
#   backslashes in their names. The game then can't find lang/english.lng and dies
#   at startup with "No available language packs". The languages were IN the zip;
#   the SEPARATOR was wrong. v0.9.1 had to be re-released as v0.9.2 just for this.
#
# THE RULE: pack with 7-Zip (or Unix `zip`), which writes forward slashes.
#   NEVER use PowerShell Compress-Archive for an Amiga/cross-platform archive.
#
# Usage (from Git Bash):   bash build/make-release-zip.sh [X.Y.Z]
#   default version is read from native/openttd/amiga_ttd_version.h.
set -e
cd "$(dirname "$0")/.."          # repo root
REPO="$(pwd)"

VER="$1"
if [ -z "$VER" ]; then
  VER=$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' native/openttd/amiga_ttd_version.h | tr -d '"')
fi
[ -n "$VER" ] || { echo "FATAL: could not determine version"; exit 1; }

SRC="$REPO/release/openttd-amiga-68k"
OUT="$REPO/openttd-amiga-68k-$VER.zip"
[ -d "$SRC" ] || { echo "FATAL: $SRC missing"; exit 1; }

SEVENZIP="/c/Program Files/7-Zip/7z.exe"
[ -x "$SEVENZIP" ] || SEVENZIP="$(command -v 7z || command -v zip)"
[ -n "$SEVENZIP" ] || { echo "FATAL: need 7-Zip or zip - do NOT fall back to Compress-Archive"; exit 1; }

echo "Packing $SRC -> $OUT with $SEVENZIP"
rm -f "$OUT"
cd "$REPO/release"
if echo "$SEVENZIP" | grep -qi "7z"; then
  "$SEVENZIP" a -tzip -mx=5 "$OUT" openttd-amiga-68k >/dev/null
else
  "$SEVENZIP" -r -q "$OUT" openttd-amiga-68k
fi

# Verify the entries use FORWARD slashes (the whole point).
echo "=== sample entries (must use '/', never '\\') ==="
if command -v unzip >/dev/null; then
  unzip -l "$OUT" | grep -E "lang/.*\.lng" | head -3
  BACKS=$(unzip -l "$OUT" | grep -c '\\' || true)
  echo "entries containing a backslash: $BACKS  (MUST be 0)"
  [ "$BACKS" = "0" ] || { echo "FATAL: backslash paths present - archive is broken for Amiga"; exit 1; }
fi
echo "DONE (zip): $OUT"

# Also make the native Amiga LHA. LHA is THE Amiga archive format and stores
# Amiga-style forward-slash paths, so it never has the backslash problem a
# Windows zip can. Built with jlha (jlha-utils, Java) in WSL. Best-effort: if
# jlha/WSL is missing, the zip still ships.
LHA="$REPO/openttd-amiga-68k-$VER.lha"
rm -f "$LHA"
if command -v wsl >/dev/null 2>&1; then
  MSYS_NO_PATHCONV=1 wsl bash -c "sudo -n mount -t drvfs I: /mnt/i 2>/dev/null; cd /mnt/i/GITHUB/Amiga_OpenTTD/release && jlha a /mnt/i/GITHUB/Amiga_OpenTTD/openttd-amiga-68k-$VER.lha openttd-amiga-68k" >/dev/null 2>&1 \
    && echo "DONE (lha): $LHA" \
    || echo "WARN: LHA not built (need 'jlha' in WSL: sudo apt-get install jlha-utils)"
else
  echo "WARN: no wsl; LHA not built"
fi
