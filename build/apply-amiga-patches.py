#!/usr/bin/env python3
"""Idempotent Amiga 68k source patches for OpenTTD 1.0.5.
Operates on the WSL build copy of the source tree. Re-runnable.
Each patch checks a marker so repeated runs are safe.
"""
import sys, os

SRC = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("OTD_SRC", "/home/angree/build/openttd-1.0.5/src")

def patch(relpath, anchor, insert_after, marker):
    path = os.path.join(SRC, relpath)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        s = f.read()
    if marker in s:
        print(f"  [skip] {relpath}: already patched ({marker!r})")
        return
    if anchor not in s:
        print(f"  [FAIL] {relpath}: anchor not found: {anchor!r}")
        sys.exit(2)
    s = s.replace(anchor, anchor + insert_after, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(s)
    print(f"  [OK]   {relpath}: applied ({marker!r})")

print("Applying Amiga 68k patches to", SRC)

# 1) unix.cpp: the __AMIGA__ CSleep block calls exec.library functions
#    (SendIO/Wait/AbortIO/WaitIO) and uses timerequest/SIGBREAKF_CTRL_C but
#    the Amiga path never included the exec/timer/dos prototypes.
patch(
    "os/unix/unix.cpp",
    "#warning add stack symbol to avoid that user needs to set stack manually (tokai)",
    "\n#include <exec/types.h>"
    "\n#include <exec/io.h>"
    "\n#include <exec/ports.h>"
    "\n#include <devices/timer.h>"
    "\n#include <dos/dos.h>"
    "\n#include <proto/exec.h>",
    "#include <proto/exec.h>",
)

# 2) crashlog_unix.cpp: libnix has no strsignal(); it is only used for crash
#    log text, so stub it to a constant string on Amiga.
patch(
    "os/unix/crashlog_unix.cpp",
    "#include <sys/utsname.h>",
    "\n\n#if defined(__AMIGA__)\n"
    "#define strsignal(sig) \"unknown\" /* libnix has no strsignal() */\n"
    "#endif",
    "#define strsignal(sig)",
)

def append_once(relpath, block, marker):
    path = os.path.join(SRC, relpath)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        s = f.read()
    if marker in s:
        print(f"  [skip] {relpath}: already appended ({marker!r})")
        return
    with open(path, "a", encoding="utf-8") as f:
        f.write(block)
    print(f"  [OK]   {relpath}: appended ({marker!r})")

# 3) newlib pulls in reentrant wrappers whose low-level syscalls _gettimeofday
#    and _link are not provided by libnix on AmigaOS -> undefined at link.
#    Provide them: gettimeofday via Amiga DateStamp(); link as ENOSYS.
append_once(
    "os/unix/unix.cpp",
    "\n\n#if defined(__AMIGA__)\n"
    "/* newlib syscall stubs not provided by libnix on AmigaOS */\n"
    "#include <sys/time.h>\n"
    "#include <errno.h>\n"
    "#include <proto/dos.h>\n"
    "extern \"C\" int _gettimeofday(struct timeval *tv, void *tz)\n"
    "{\n"
    "\t(void)tz;\n"
    "\tif (tv != 0) {\n"
    "\t\tstruct DateStamp ds;\n"
    "\t\tDateStamp(&ds);\n"
    "\t\ttv->tv_sec  = (long)ds.ds_Days * 86400L + (long)ds.ds_Minute * 60L + (long)ds.ds_Tick / 50L;\n"
    "\t\ttv->tv_usec = ((long)ds.ds_Tick % 50L) * 20000L;\n"
    "\t}\n"
    "\treturn 0;\n"
    "}\n"
    "extern \"C\" int _link(const char *a, const char *b)\n"
    "{\n"
    "\t(void)a; (void)b; errno = ENOSYS; return -1;\n"
    "}\n"
    "#endif /* __AMIGA__ */\n",
    "int _gettimeofday(struct timeval *tv, void *tz)",
)

# 4) OpenTTD is a large C++ program with deep call stacks (pathfinder, GUI,
#    saveload). The default AmigaOS shell stack (~4-8KB) overflows instantly.
#    libnix honors a global `__stack` to request a bigger stack, like the
#    MorphOS port's `ULONG __stack = (1024*1024)*2`.
append_once(
    "os/unix/unix.cpp",
    "\n#if defined(__AMIGA__)\n"
    "unsigned long __stack = 1024 * 1024; /* OpenTTD needs a large stack on AmigaOS */\n"
    "#endif\n",
    "unsigned long __stack = 1024",
)

# ---------------------------------------------------------------------------
# config.lib patches (tree root, i.e. SRC/..) — needed by BOTH the dedicated
# and the SDL/non-dedicated build; formerly applied by an ephemeral scratchpad
# script. Reference diff: build/patches/openttd-1.0.5-configlib-amiga.patch
# NOTE (Phase 5b): the non-dedicated SDL build MUST be configured with
# --enable-network=0. ENABLE_NETWORK drags <proto/exec.h> (-> bebbo
# inline/stubs.h -> dos/intuition/graphics headers + Insert/Remove/Allocate
# function-macros) into every TU via network/core/os_abstraction.h, and
# 1.0.5's network code needs sockaddr_storage/getaddrinfo which the classic
# bsdsocket SDK lacks. See build/BUILD-NOTES-sdl.md §6 and
# build/patches/sdl-build-nondedicated.sh.

def patch_at(path, old, new):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        s = f.read()
    if new in s:
        print(f"  [skip] {os.path.basename(path)}: already patched ({new[:40]!r})")
        return
    if old not in s:
        print(f"  [FAIL] {path}: anchor not found: {old!r}")
        sys.exit(2)
    s = s.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(s)
    print(f"  [OK]   {os.path.basename(path)}: {new[:40]!r}")

CONFIG_LIB = os.path.join(os.path.dirname(SRC.rstrip("/")), "config.lib")
if os.path.exists(CONFIG_LIB):
    # 1) gcc >= 7 prints "11" etc. for -dumpversion; the stock `cut -c 1,3`
    #    yields "1" -> "gcc older than 3.3" abort. (Affects the BUILD gcc too.)
    patch_at(CONFIG_LIB,
        "cc_version=`$1 -dumpversion | cut -c 1,3`",
        "cc_version=`$1 -dumpversion | cut -d. -f1`0")
    # 2) keep -std=gnu++98 for the target (gnu++0x on GCC 6.5 changes squirrel
    #    behavior; the port is C++03).
    patch_at(CONFIG_LIB,
        'cxxflags="$cxxflags -std=gnu++0x"',
        'if [ "$host" != "m68k-amigaos" ]; then cxxflags="$cxxflags -std=gnu++0x"; fi')
    # 3) no -lpthread (no threads) and no explicit -lc: -lc pulls newlib libc
    #    next to libnix (-noixemul) -> multiple definition of close/fclose/__errno.
    patch_at(CONFIG_LIB,
        'LIBS="$LIBS -lpthread"',
        'if [ "$host" != "m68k-amigaos" ]; then LIBS="$LIBS -lpthread"; fi')
    patch_at(CONFIG_LIB,
        'LIBS="$LIBS -lc"',
        'if [ "$host" != "m68k-amigaos" ]; then LIBS="$LIBS -lc"; fi')
else:
    print(f"  [note] {CONFIG_LIB} not found; skipping config.lib patches")

print("Done.")
