#!/bin/bash
# ============================================================================
# Build OpenTTD for AmigaOS 68k with NO dependency on an FPU.
#
# WHY -mcpu=68020 -msoft-float AND NOT -m68040 -msoft-float
# ---------------------------------------------------------
# bebbo's driver maps -m68040 onto the 68020+68881 multilib, because a 68040
# has an FPU built in. -msoft-float does NOT undo that. You end up with a
# binary whose own code respects the soft-float ABI but whose C library was
# taken from .../libnix/lib/libm020/libm881/ - and libnix's printf engine,
# __vfprintf_total_size, hoists two double constants and an fmovem into its
# PROLOGUE:
#
#     lea      sp@(-92),sp
#     fmovemx  %fp2/%fp4/%fp6,sp@-      <- unconditional
#     fmoved   #1e-10,%fp4              <- unconditional
#     fmoved   #-0.1,%fp2               <- unconditional
#
# Those run on EVERY printf/snprintf call, whatever the format string. On a CPU
# without an FPU the first snprintf("%d") is a Line-F trap - Software Failure
# #8000000B - long before anything can be logged, which is why the symptom was
# "dies at startup with a 0-byte log" and looked like a map-generation problem.
#
# Proven both ways with a 30-line probe on a 68030/no-FPU emulator config:
# built -m68040 it dies inside snprintf; built -mcpu=68020 -msoft-float it
# completes. See docs/NOFPU.md.
#
# -mcpu=68020 selects .../libm020/ (no 881), where every library object really
# is soft-float. Check with:  m68k-amigaos-g++ <flags> ... -Wl,-t | grep libm881
# If libm881 appears in that trace, the build still needs an FPU.
#
# NOTE the float ABI change invalidates EVERY object, including the ones that
# must be compiled at -O0. That is why this goes through build-with-ice-retry.sh
# and never calls make directly - see the warnings in that script.
# ============================================================================
set -e
export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin

TREE=/home/angree/build/openttd-1.0.5
MK="$TREE/objs/release/Makefile"
BUILD_SCRIPT="${1:-/tmp/bbuild.sh}"
ZLIB=/home/angree/build/zlib-1.2.13
FLAGS="-mcpu=68020 -msoft-float -noixemul"

cd "$TREE"

echo "== prerequisites =="
if ! grep -q 'settings_gui.o' "$BUILD_SCRIPT"; then
    echo "REFUSING: $BUILD_SCRIPT has a stale FORCE_O0 list (no settings_gui.o)"
    exit 1
fi

echo "== switching the tree to $FLAGS =="
# Must be idempotent: this script gets re-run after a failure. Rewriting
# unconditionally would strip -msoft-float on the second pass and then find no
# -m68040 to put it back, quietly producing a hard-float build again.
if grep -qE '^(CFLAGS|CXXFLAGS|LDFLAGS) .*-m68040' "$MK"; then
    # Strip any existing float flag FIRST, then rewrite -m68040 (which
    # reintroduces -msoft-float), otherwise the flags end up duplicated.
    sed -i 's/-mhard-float//g; s/-msoft-float//g; s/-m68040/-mcpu=68020 -msoft-float/g' "$MK"
    echo "-- flags rewritten"
else
    echo "-- flags already switched, leaving them alone"
fi
echo "-- CFLAGS now:"
grep -m1 "^CFLAGS " "$MK" | tr ' ' '\n' | grep -E "float|68020|68040|^-O" || true
echo "-- LDFLAGS now:"
grep -m1 "^LDFLAGS " "$MK" | tr ' ' '\n' | grep -E "float|68020|68040|^-O" || true
for V in CFLAGS CXXFLAGS LDFLAGS; do
    L=$(grep -m1 "^$V " "$MK")
    case "$L" in *-m68040*) echo "REFUSING: -m68040 still in $V"; exit 1;; esac
    case "$L" in *-mcpu=68020*) ;; *) echo "REFUSING: -mcpu=68020 missing from $V"; exit 1;; esac
    case "$L" in *-msoft-float*) ;; *) echo "REFUSING: -msoft-float missing from $V"; exit 1;; esac
done

echo "== rebuilding zlib at -O0 with the new ABI =="
# zlib must stay at -O0: at -O1 deflateInit fails, at -O2 uncompress silently
# returns corrupt data. Its adler32_combine64 contains float code, so it has to
# follow the ABI change like everything else.
cd "$ZLIB"
rm -f *.o libz.a
m68k-amigaos-gcc -O0 $FLAGS -I. -c adler32.c compress.c crc32.c deflate.c \
  gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c \
  inftrees.c trees.c uncompr.c zutil.c
m68k-amigaos-ar rcs libz.a *.o
m68k-amigaos-ranlib libz.a
ls -la "$ZLIB/libz.a"

# Link that exact archive rather than trusting -lz to pick the right multilib.
if grep -q ' \-lz' "$MK"; then
    sed -i "s| -lz| $ZLIB/libz.a|" "$MK"
    echo "-- LIBS now points at the freshly built zlib"
fi

echo "== rebuilding the hand-built objects with the matching ABI =="
# These are linked in through LIBS, so make never rebuilds them. If their float
# ABI does not match the rest, arguments are passed differently across that
# boundary. c2p_glue.o is pure assembly with no float and is left alone.
# amiga_gfx.c needs the CyberGraphX headers, which live in the tree (they are
# third-party and deliberately kept out of the published diff).
cd "$TREE/src/video" && m68k-amigaos-gcc -O1 $FLAGS -Icgx-include -c -o amiga_gfx.o amiga_gfx.c
cd "$TREE/src/sound" && m68k-amigaos-gcc -O1 $FLAGS -c -o amiga_audio.o amiga_audio.c
cd "$TREE/src"       && m68k-amigaos-gcc -O1 $FLAGS -c -o fp_conv.o fp_conv.c
cd "$TREE"
ls -la src/video/amiga_gfx.o src/sound/amiga_audio.o src/fp_conv.o

echo "== a float ABI change invalidates every object =="
find "$TREE/objs" -name '*.o' -delete
rm -f "$TREE/bin/openttd" "$TREE/objs/release/openttd"

echo "== full build (forces the known-bad-codegen files to -O0) =="
bash "$BUILD_SCRIPT"

echo "== verifying the result executes no FPU instruction =="
N=$(m68k-amigaos-objdump -d "$TREE/bin/openttd" 2>/dev/null \
    | grep -cE '	f(move|add|sub|mul|div|cmp|tst|neg|abs|int|save|restore|sgl|dmove)')
echo "FPU instructions in the linked binary: $N"
if [ "$N" != "0" ]; then
    echo "-- they live in these symbols:"
    m68k-amigaos-objdump -d "$TREE/bin/openttd" 2>/dev/null | awk -F'\t' '
      /^[0-9a-f]+ </ { s=$0; sub(/^[0-9a-f]+ </,"",s); sub(/>:.*$/,"",s); cur=s }
      NF>=3 { split($3,a," "); m=a[1];
        if (m ~ /^f(move|add|sub|mul|div|cmp|tst|neg|abs|int|save|restore|sgl|dmove)/) c[cur]++ }
      END { for (s in c) printf "%6d  %s\n", c[s], s }' | sort -rn | head -20
fi
ls -la "$TREE/bin/openttd"
