#!/usr/bin/env python3
"""Re-apply the PC-side portability fixes to a freshly synced PC tree.

Every fix is idempotent: if the patched form is already present, it is skipped.
These are the ONLY src/ edits the PC build needs on top of the Amiga sources.
They are candidates for porting back into the shared sources (all three are
harmless on m68k), but they live here so the Amiga tree is never modified.

Usage: pc-fixes.py <tree-root>
"""
import sys
import os

tree = sys.argv[1]


def patch(relpath, old, new, why):
    p = os.path.join(tree, relpath)
    if not os.path.exists(p):
        print("  skip (missing): %s" % relpath)
        return
    s = open(p).read()
    # "Is it already patched?" is NOT "does the new text appear" - the new text
    # can be a SUBSTRING of the old one. Fix 1 replaces a guarded #include with
    # a bare one, so the bare form is present either way and every run reported
    # "already patched" while leaving the guard in place. The PC build then died
    # with getcwd/chdir undeclared, hours after the last real change. Test for
    # the OLD text first: if it is still there, the fix has not been applied.
    if old not in s:
        if new in s:
            print("  already patched: %s" % relpath)
            return
        print("  !! PATTERN NOT FOUND in %s (%s) - check upstream change" % (relpath, why))
        sys.exit(1)
    open(p, "w").write(s.replace(old, new, 1))
    print("  patched: %s  (%s)" % (relpath, why))


print("=== pc-fixes: %s ===" % tree)

# 1. glibc >= 2.32 no longer pulls <unistd.h> in via <pwd.h>, so getcwd/chdir/
#    getuid are undeclared on a modern Linux host. The Amiga port had already
#    narrowed this include to OPENBSD/DOS/__AMIGA__; just make it unconditional.
patch("src/fileio.cpp",
      "#if defined(OPENBSD) || defined(DOS) || defined(__AMIGA__)\n#include <unistd.h>\n#endif",
      "#include <unistd.h>",
      "unistd.h for getcwd/chdir/getuid on glibc >= 2.32")

# 2. mingw-w64 provides a C99 snprintf; the 2010 version gate only knew about
#    mingw.org and so redefined it, which is a hard error.
patch("src/string.cpp",
      "#if (__MINGW32_MAJOR_VERSION < 3) || ((__MINGW32_MAJOR_VERSION == 3) && (__MINGW32_MINOR_VERSION < 14))",
      "/* mingw-w64 always provides a C99 snprintf; only ancient mingw.org needs this. */\n"
      "#if !defined(__MINGW64_VERSION_MAJOR) && ((__MINGW32_MAJOR_VERSION < 3) || ((__MINGW32_MAJOR_VERSION == 3) && (__MINGW32_MINOR_VERSION < 14)))",
      "do not redefine snprintf on mingw-w64")

# 3. The gcc branch of the crash handler saved a 32-bit %esp with a hand-written
#    symbol name (__safe_esp, win32 underscore mangling). On win64 that is both
#    the wrong register width and the wrong symbol.
patch("src/os/windows/crashlog_win.cpp",
      '#else\n\tasm("movl %esp, __safe_esp");\n#endif',
      '#elif defined(__x86_64__)\n'
      '\t/* win64 gcc: 64-bit stack pointer, and no leading underscore on symbols.\n'
      "\t * Use an operand constraint so the mangling is the assembler's problem. */\n"
      '\tasm("movq %%rsp, %0" : "=m" (_safe_esp));\n'
      '#else\n'
      '\tasm("movl %esp, __safe_esp");\n'
      '#endif',
      "win64 stack-pointer save in the crash handler")

print("=== pc-fixes done ===")
