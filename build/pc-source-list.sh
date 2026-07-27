#!/bin/bash
# Rewrite the Amiga source.list into a PC one (stdin -> stdout).
#  - drops Amiga-only drivers (amiga_v.cpp / amiga_s.cpp and their headers)
#  - re-enables mixer.cpp (on Amiga it is replaced by sound/amiga_s.cpp)
#  - adds our custom AI, which on Amiga is hand-compiled into LIBS.
#    NOTE: oldai_pathfinder.cpp is #included by oldai.cpp - never compile it alone.
#  - adds the two PC-only files that live in the PC tree only:
#      ai_old/oldai_log_pc.c  (dos.library logger -> stdio)
#      pc_amiga_stubs.cpp     (AmigaMemProbe / music scanner / MxSetNextSoundID)
sed -e "/^video\/amiga_v\.cpp$/d" \
    -e "/^sound\/amiga_s\.cpp$/d" \
    -e "/^video\/amiga_v\.h$/d" \
    -e "/^video\/amiga_gfx\.h$/d" \
    -e "/^sound\/amiga_s\.h$/d" \
    -e "/^sound\/amiga_audio\.h$/d" \
    -e "s|^# mixer\.cpp .*|mixer.cpp|" \
    -e "s|^music\.cpp$|music.cpp\nai_old/oldai.cpp\nai_old/oldai_log_pc.c\nai_old/oldai.h\npc_amiga_stubs.cpp|"
