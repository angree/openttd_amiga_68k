#!/bin/bash
# Native MorphOS PPC build of AmiTTD from the SAME sources as the 68k build.
# Two WSL distributions, because morphos-cross needs glibc >= 2.36 and the 68k
# tree lives in Ubuntu-22.04. From PowerShell:
#   wsl -d Ubuntu-22.04 -- sh /mnt/i/GITHUB/Amiga_OpenTTD/build/morphos-snapshot.sh
#   wsl -d Ubuntu-24 -u root -- bash /mnt/i/GITHUB/Amiga_OpenTTD/build/build-morphos.sh fresh
# The snapshot is the patched 68k tree (src.tgz), its source list and zlib; this
# script compiles the same 318 game files plus our platform files with
# ppc-morphos-gcc 15 (metaneutrons/morphos-cross) and links openttd-mos.
# Output: $B/src/openttd-mos, copied to $P/openttd-mos.
export PATH=/root/.morphos-cross/current/bin:/usr/local/bin:/usr/bin:/bin
P=/mnt/c/temp/amittd-mos/ppc
B=/root/ottd-mos
NAT=/mnt/i/GITHUB/Amiga_OpenTTD/native/openttd
ls /mnt/i/GITHUB >/dev/null 2>&1 || mount -t drvfs I: /mnt/i

if [ "$1" = fresh ] || [ ! -d $B/src ]; then
	rm -rf $B; mkdir -p $B
	tar xzf $P/src.tgz -C $B
	tar xzf $P/zlib.tgz -C $B
fi
# Our platform files always come straight from the repo.
cp $NAT/amiga_gfx.c $NAT/amiga_gfx.h $NAT/amiga_ehb_data.h $NAT/amiga_v.cpp $NAT/amiga_v.h $B/src/video/
cp $NAT/amiga_audio.c $NAT/amiga_audio.h $NAT/amiga_adpcm.c $NAT/amiga_adpcm.h $NAT/amiga_s.cpp $NAT/amiga_s.h $B/src/sound/
cp $NAT/amiga_m.cpp $NAT/amiga_m.h $NAT/amiga_mscan.c $NAT/amiga_mscan.h $NAT/amiga_camd.c $NAT/amiga_camd.h $B/src/music/
cp $NAT/ai_old/oldai.cpp $NAT/ai_old/oldai_pathfinder.cpp $NAT/ai_old/oldai.h $NAT/ai_old/oldai_log.c $B/src/ai_old/
cp $NAT/amiga_ttd_version.h $B/src/
cp $NAT/mos_extra.c $B/src/

cd $B/src
DEFS='-DENABLE_NETWORK -DUNIX -DNO_THREADS -DWITH_ZLIB -DENABLE_AI -DNDEBUG -DWITH_PERSONAL_DIR -DPERSONAL_DIR="\".openttd\"" -DGLOBAL_DATA_DIR="\"/usr/local/share/games/openttd\""'
INC="-I$B/zlib-1.2.13 -I$B/src/3rdparty/squirrel/include -I$B/objs/release -I$B/objs/lang"
# -fsigned-char is NOT optional: char is unsigned on PPC and signed on 68k and
# x86, and a multiplayer game between them must compute the same thing.
OPT="-O2 -fno-strict-aliasing -fwrapv -fsigned-char -noixemul -w"
CXXF="-std=gnu++98 -fpermissive"

{
	echo "OBJS :="
	grep -v '^ai_old/oldai.cpp$' $P/srcs.txt | tr -d '\r' | while read s; do
		o=${s%.cpp}.mo
		echo "OBJS += $o"
		echo "$o: $s"
		echo "	ppc-morphos-g++ $OPT $CXXF $DEFS $INC -c \$< -o \$@ 2> \$@.err || (cat \$@.err; false)"
	done
	for s in ai_old/oldai.cpp music/amiga_m.cpp; do
		o=${s%.cpp}.mo
		grep -qx "$s" $P/srcs.txt && continue
		echo "OBJS += $o"
		echo "$o: $s"
		echo "	ppc-morphos-g++ $OPT $CXXF $DEFS $INC -c \$< -o \$@ 2> \$@.err || (cat \$@.err; false)"
	done
	for s in ai_old/oldai_log.c video/amiga_gfx.c sound/amiga_audio.c sound/amiga_adpcm.c music/amiga_mscan.c music/amiga_camd.c mos_extra.c; do
		[ -f $s ] || continue
		o=${s%.c}.mo
		echo "OBJS += $o"
		echo "$o: $s"
		echo "	ppc-morphos-gcc $OPT $DEFS $INC -c \$< -o \$@ 2> \$@.err || (cat \$@.err; false)"
	done
	# zlib linked statically from the same 1.2.13 sources the 68k build uses,
	# so the game does not depend on the version of z.library installed.
	for s in $(cd $B && ls zlib-1.2.13/*.c | grep -v -e example -e minigzip); do
		o=${s%.c}.mo
		echo "OBJS += ../$o"
		echo "../$o: ../$s"
		echo "	ppc-morphos-gcc -O2 -noixemul -w -DHAVE_UNISTD_H -c \$< -o \$@"
	done
	echo "openttd-mos: \$(OBJS)"
	echo "	ppc-morphos-g++ -noixemul -o \$@ \$(OBJS) -lm"
} > Makefile.mos

make -k -j8 -f Makefile.mos openttd-mos > $P/mos-build.log 2>&1
rc=$?
echo "make rc=$rc"
grep -c "error:" $P/mos-build.log
ls -la openttd-mos 2>/dev/null && file openttd-mos && cp openttd-mos $P/openttd-mos
exit $rc
