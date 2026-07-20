#!/bin/bash
# Build OpenTTD for AmigaOS 68k.
#
# Two separate reasons a file may have to drop to -O0:
#
#  1. FORCE_O0 below - files that COMPILE at -O1 but produce broken code.
#     order_cmd.cpp / order_gui.cpp: adding an order to a vehicle silently does
#     nothing at -O1. It worked while diagnostic logging was present and broke
#     again the moment the logging was removed - a known symptom here, because
#     this compiler hoists loads across function calls at -O1, so an extra
#     statement between a call and a read changes the generated code.
#     These are not optional and they are not ICEs, so nothing detects them
#     automatically. Any change to CFLAGS invalidates every object and silently
#     rebuilds these at -O1 again, which is exactly how the window.cpp bug came
#     back once already.
#
#     window.cpp  - at -O1, closing any OpenTTD window kills the game with an
#                   uncaught C++ exception.
#     cargotype.cpp - at -O1, GCC strength-reduces the label search loop in
#                   SetupCargoForClimate() into "cmp.l (a0)+,a4" followed by a
#                   separate "add.w #56,a0". The post-increment already advanced
#                   a0 by 4, so each iteration walks 60 bytes through an array of
#                   56-byte CargoSpec structs and the read address drifts +4 per
#                   element. Only j=0 (peeled by the compiler) and j=1 ever match,
#                   so every cargo except PASS and COAL comes out invalid. That
#                   breaks industry cargo names AND RoadVehicle::IsBus(), which
#                   reads CargoSpec::classes, so buses are mistaken for trucks
#                   and refuse to accept an order to a bus stop.
#
#  2. The cc1plus internal compiler error, handled by the retry loop.
#
# Never -O2 anywhere: that level breaks C++ exception unwinding on this
# toolchain and the game dies on its first frame.

export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin
TREE=/home/angree/build/openttd-1.0.5
FORCE_O0="window.o cargotype.o order_cmd.o order_gui.o settings_gui.o rail_cmd.o signal.o"

cd "$TREE" || exit 1

rebuild_at_O0() {
    # $1 = object path relative to objs/release
    cd "$TREE/objs/release" || exit 1
    rm -f "$1"
    make -n "$1" 2>/dev/null | grep -m1 "m68k-amigaos-g" > /tmp/ice_cmd.txt
    if [ ! -s /tmp/ice_cmd.txt ]; then
        echo "could not recover the compile command for $1"; cd "$TREE"; return 1
    fi
    sed -i 's/-O1/-O0/g' /tmp/ice_cmd.txt
    bash /tmp/ice_cmd.txt || { echo "still failed at -O0: $1"; cd "$TREE"; return 1; }
    cd "$TREE" || exit 1
    return 0
}

for attempt in $(seq 1 12); do
    OUT=$(make 2>&1)
    if echo "$OUT" | grep -q "internal compiler error"; then
        # Take the full source path from the error and strip everything up to
        # and including the last /src/. Deriving it any other way is fragile:
        # the tree name contains dots, and the object may not exist yet when
        # the compile is what failed.
        SRCPATH=$(echo "$OUT" | grep -B3 "internal compiler error" | grep -oE "/[A-Za-z0-9_/.-]+\.cpp" | tail -1)
        REL=${SRCPATH##*/src/}
        OBJ=${REL%.cpp}.o
        BASE=${REL##*/}
        # Dropping a file to -O0 is fine for game logic, but NOT for code that
        # runs every frame. amiga_v.cpp is the video driver: event polling, the
        # dirty-rectangle bookkeeping and the blit. It was silently degraded to
        # -O0 by this very fallback once (an ICE in MakeDirty), which made the
        # game unplayable - and it stayed that way for several builds, because
        # make does not recompile an unchanged file, so every later link quietly
        # reused the crippled object. Refuse instead of degrading: the fix is to
        # put __attribute__((optimize("O0"))) on the ONE offending function and
        # leave the rest of the file optimised.
        case "$BASE" in
            amiga_v.cpp|amiga_gfx.c)
                echo "REFUSING: ICE in $BASE, which runs every frame."
                echo "  Dropping it to -O0 costs real frame time and has shipped an"
                echo "  unplayable build before. Mark the single offending function"
                echo "  with __attribute__((optimize(\"O0\"))) instead, then rebuild."
                exit 1
                ;;
        esac
        echo "[$attempt] ICE in $BASE -> rebuilding at -O0"
        rebuild_at_O0 "$OBJ" || exit 1
        continue
    fi
    if echo "$OUT" | grep -qE "error:|Error 1"; then
        echo "BUILD ERROR:"; echo "$OUT" | grep -E "error:|Error 1" | head -5; exit 1
    fi
    break
done

# Now force the known-bad-codegen files to -O0 and relink. Done after the main
# build so the objects definitely exist and the compile line can be recovered.
NEED_RELINK=0
FORCED_REPORT=""
for OBJ in $FORCE_O0; do
    echo "forcing $OBJ to -O0 (miscompiled at -O1)"
    rebuild_at_O0 "$OBJ" || exit 1
    # Confirm from the actual command line that -O0 is what was used, per object.
    if grep -q -- "-O0" /tmp/ice_cmd.txt; then
        FORCED_REPORT="$FORCED_REPORT $OBJ=-O0"
    else
        echo "REFUSING: $OBJ was not rebuilt at -O0"; exit 1
    fi
    NEED_RELINK=1
done

if [ "$NEED_RELINK" = "1" ]; then
    rm -f "$TREE/bin/openttd" "$TREE/objs/release/openttd"
    make 2>&1 | grep -E "error:|Error 1" | head -5
fi

ls -la "$TREE/bin/openttd" || exit 1
od -An -tx1 -N4 "$TREE/bin/openttd"
echo "forced-O0 objects:$FORCED_REPORT"
# Kept for grep-compatibility with older notes/scripts.
echo "window.o built at: -O0"
