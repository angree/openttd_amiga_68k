# Building

Cross-compiled with [bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc)
(GCC 6.5.0b) against an unpacked OpenTTD 1.0.5 source tree.

## Toolchain traps

These cost far more time than the port itself. All four produce **working
binaries that behave wrongly**, not compile errors, so they are easy to mistake
for bugs in the game or in the driver. Each was isolated with a small standalone
Amiga program rather than by debugging inside the 5 MB binary — the test
programs are kept in `native/` for exactly that reason.

### 1. `-O2` breaks C++ exception unwinding

`native/exctest.cpp` throws and catches, four frames deep:

| build | result |
|---|---|
| `-O0` | catches both, runs to completion |
| `-O1` | same, fine |
| `-O2` | catches the throw, then dies with Software Failure #80000003 |
| `-O2 -fno-omit-frame-pointer` | still dies, so the frame pointer is not the cause |

OpenTTD throws and catches by design (`SlError` in `saveload.cpp`), so at `-O2`
the game dies on its first frame — and a `catch (...)` placed directly around the
call does not catch it, because unwinding phase 1 never finds the handler.

**Build the whole tree at `-O1`.** Where a file trips the cc1plus ICE, fall back
to `-O0`, never `-O2`.

### 2. `-m68040` does not mean hardware FPU

You must pass **`-mhard-float`** explicitly. Without it GCC emits soft-float, and
its double routines are broken — identically at `-O0`, `-O1` and `-O2`, which is
why dropping the optimisation level never helps. `native/fptest.c`:

```
pow(0.7,1) = 0.490000   (want 0.7)
pow(0.7,3) = 0.000000   (want 0.343)
```

`sin()` and the basic operators were fine; `pow()` and double-to-string were
garbage. This broke two things that looked unrelated: map generation hung
(`tgp.cpp` calls `pow()` in its inner loop) and savegames written by that binary
were permanently corrupt — they load, then fail `AfterLoadGame()`.

Only terrain generation needs floating point. The simulation is integer-only by
design, because OpenTTD keeps multiplayer deterministic across platforms.

### 3. zlib must be built at `-O0`

Same compiler, different failure mode, and again silent:

| zlib built at | result |
|---|---|
| `-O0` | correct, full compress/decompress round trip |
| `-O1` | `deflateInit` returns `-2 Z_STREAM_ERROR` |
| `-O2` | compresses, but `uncompress` returns `-3` — **silent data corruption** |
| `-O1 -fno-strict-aliasing` | still broken, so aliasing is not the cause |

The fault is in the library, not the caller. Cross-testing settles it: caller
`-O0` + zlib `-O1` fails, caller `-O1` + zlib `-O0` works. So the game stays at
`-O1` and only zlib drops to `-O0`. `native/zlibtest.c` reports the exact return
codes and the ABI both sides agree on.

```
m68k-amigaos-gcc -O0 -m68040 -mhard-float -noixemul -I. -c adler32.c compress.c \
  crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c \
  inflate.c inftrees.c trees.c uncompr.c zutil.c
m68k-amigaos-ar rcs libz.a *.o && m68k-amigaos-ranlib libz.a
```

### 4. `window.cpp` miscompiles at `-O1`

Closing any OpenTTD window crashed instantly with an uncaught C++ exception.
Rebuilding just `window.cpp` at `-O0` fixes it. This is a workaround, not a
diagnosis — `-O0` on the whole GUI module is slow, and the right fix is to find
the individual pass responsible.

## Other things worth knowing

- **`./configure` flags do not reach `objs/release/Makefile`.** The configure
  script passed `-mhard-float` and the generated Makefile had only `-m68040`.
  Always verify flags by grepping the generated Makefile.
- **bebbo defines `__AMIGA__`, not `__AMIGAOS__`.** Every in-tree OpenTTD path
  guarded on `__AMIGAOS__` alone is dead code, including directory creation and
  `Volume:` path handling — which is why saving hung until it was fixed.
- **`CSleep()` dereferences NULL.** OpenTTD's own Amiga implementation drives
  `timer.device` through `TimerPort`/`TimerRequest`, which `unix.cpp` declares as
  NULL and nothing ever opens. Replaced with dos.library `Delay()`.
- **OpenTTD logs to stderr, which the AmigaDOS 3.1 shell cannot redirect.** Patch
  `debug.cpp` to use `stdout` with `fflush`, or you will be debugging blind.
  `freopen("Work:ottd.log", stderr)` silently fails.
- **Never run with a bare `-d 3`.** It raises the desync debug level, which
  enables `CheckCaches()` and kills the game at `roadstop.cpp:389` for reasons
  unrelated to any real bug. Use targeted channels: `-d driver=2,grf=1,sl=1`.
- The assembly c2p is assembled with `vasmm68k_mot`, which ships with amiga-gcc,
  so Kalms' Motorola-syntax source needs no translation:
  `vasmm68k_mot -Fhunk -m68040 -no-opt -o c2p_glue.o c2p_glue.s`

## Configure line

```
./configure \
  --host=m68k-amigaos \
  --cc-build=gcc --cxx-build=g++ \
  --cc-host=m68k-amigaos-gcc --cxx-host=m68k-amigaos-g++ \
  --strip=m68k-amigaos-strip \
  --os=UNIX --endian=BE \
  --enable-network=0 --without-threads --disable-strip --disable-unicode \
  --without-png --without-freetype --without-fontconfig \
  --without-icu --without-lzo2 \
  --without-sdl --without-allegro --without-iconv \
  CFLAGS="-m68040 -mhard-float -noixemul -DUNIX" \
  CXXFLAGS="-m68040 -mhard-float -noixemul -std=gnu++98 -DUNIX" \
  LDFLAGS="-noixemul -m68040 -mhard-float"
```

Then fix up `objs/release/Makefile`: optimisation to `-O1` in `CFLAGS` and
`LDFLAGS`, `-fpermissive` on `CXXFLAGS`, and add the prebuilt `amiga_gfx.o` and
`c2p_glue.o` to `LIBS` (no `-lpthread`, no `-lc` — either pulls newlib in
alongside libnix and you get duplicate symbols).

## Testing

`winuae/` holds WinUAE configurations and host-side automation. The default
config is throttled to roughly a 68040/40; there is a separate max-speed one for
when raw throughput matters more than realism. Calibrate with SysInfo — WinUAE
writes its CPU speed slider in **tenths of a percent** (`cpu_throttle=-700.0`
is -70%), and `cpu_speed=real` silently pins the machine to A500 timing while
ignoring `cpu_frequency`.
