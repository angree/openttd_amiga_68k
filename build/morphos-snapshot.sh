#!/bin/sh
# Ubuntu-22.04 half of the MorphOS PPC build (see build-morphos.sh): packs the
# patched 68k tree, its source list and the zlib sources into
# /mnt/c/temp/amittd-mos/ppc for the Ubuntu-24 half to compile.
set -e
cd /home/angree/build/openttd-1.0.5
mkdir -p /mnt/c/temp/amittd-mos/ppc
sed -n 's/^CFLAGS *= *//p' objs/release/Makefile > /mnt/c/temp/amittd-mos/ppc/cflags.txt
sed -n 's/^CXXFLAGS *= *//p' objs/release/Makefile > /mnt/c/temp/amittd-mos/ppc/cxxflags.txt
sed -n 's/^SRCS *:= *//p' objs/release/Makefile | tr ' ' '\n' | grep . > /mnt/c/temp/amittd-mos/ppc/srcs.txt
sed -n 's/^LIBS *= *//p' objs/release/Makefile > /mnt/c/temp/amittd-mos/ppc/libs.txt
ls objs/release/*.h objs/release/*/*.h 2>/dev/null | head; ls objs/lang 2>/dev/null | head -3
tar czf /mnt/c/temp/amittd-mos/ppc/src.tgz src objs/release/*.h objs/lang/table 2>/dev/null || tar czf /mnt/c/temp/amittd-mos/ppc/src.tgz src
ls -la /mnt/c/temp/amittd-mos/ppc
ls /home/angree/build/zlib-1.2.13 | head -50 | tr '\n' ' '
(cd /home/angree/build && tar czf /mnt/c/temp/amittd-mos/ppc/zlib.tgz zlib-1.2.13/*.c zlib-1.2.13/*.h)
