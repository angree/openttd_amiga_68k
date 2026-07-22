# Our changes to OpenTTD's own source

`amiga-68k.diff` is the complete difference between pristine OpenTTD 1.0.5 and
the tree these binaries are built from. Together with `native/`, it is the whole
of what makes this port — and it is what GPL v2 requires us to publish alongside
the binaries we ship.

It is generated mechanically rather than written by hand, because it has to be
complete: an enumerated list of patches is only as good as someone's memory, and
a silently missing one here does not fail the build, it produces a subtly broken
game.

## Applying it

```sh
tar xf openttd-1.0.5-source.tar.bz2
cd openttd-1.0.5
patch -p1 --directory=src < ../patches/amiga-68k.diff
```

Then copy the driver sources from `native/openttd/` into place — they are kept
separately because they are wholly new files rather than modifications:

```sh
cp native/openttd/amiga_v.cpp   native/openttd/amiga_v.h     src/video/
cp native/openttd/amiga_gfx.c   native/openttd/amiga_gfx.h   src/video/
cp native/openttd/amiga_s.cpp   native/openttd/amiga_s.h     src/sound/
cp native/openttd/amiga_audio.c native/openttd/amiga_audio.h src/sound/
cp native/openttd/amiga_adpcm.c native/openttd/amiga_adpcm.h src/sound/
mkdir -p src/music && cp native/openttd/amiga_m.cpp native/openttd/amiga_m.h src/music/
mkdir -p src/ai_old && cp native/openttd/ai_old/oldai.cpp native/openttd/ai_old/oldai.h \
                          native/openttd/ai_old/oldai_log.c src/ai_old/
cp native/openttd/fp_conv.c    src/
cp native/c2p1x1_6_c5_bm_040.s native/c2p_rect.s native/c2p_glue.s src/video/
```

`amiga_m.o`, `amiga_adpcm.o`, `oldai.o` and `oldai_log.o` are compiled by hand
(plain C at `-O0`, C++ at `-O0`) and linked via `LIBS` in
`objs/release/Makefile`, exactly like `amiga_audio.o` / `amiga_gfx.o` — they are
not in `source.list`. See `build/` for the helper scripts.

## One thing you have to supply yourself

The RTG (Picasso96 / CyberGraphX) path needs the **CyberGraphX developer
headers** — `cybergraphx/cybergraphics.h`, `clib/cybergraphics_protos.h`,
`inline/cybergraphics.h`, `proto/cybergraphics.h`. They are not in bebbo's
toolchain and they are not in this repository: they carry
"Copyright © 1996-1998 by phase5 digital products" and we have no
redistribution licence to point at. Get them from the CyberGraphX or Picasso96
developer kit and put them in `src/video/cgx-include/`.

Two of them need fixing before they will compile with GCC 6.5, which is worth
knowing before you lose an hour to it: the FD2Inline-generated
`inline/cybergraphics.h` declares a register variable with no type and lists the
same register as both an output operand and a clobber, and `proto/cybergraphics.h`
pulls in the `clib` prototypes and the `inline` definitions at the same time,
which gives you a screenful of "static declaration follows non-static
declaration". Real NDK headers pick one or the other.

Without these headers the AGA path still builds and runs; only RTG is lost.

Read [../BUILDING.md](../BUILDING.md) before building. The toolchain has several
traps that produce silently wrong code rather than errors, and
`build/build-with-ice-retry.sh` is the only correct way to build — a plain
`make` will quietly produce a broken game.

## What is in here, roughly

- **`os/unix/*`, `debug.cpp`, `fileio.cpp`, `fios.cpp`, `string_func.h`,
  `network/core/os_abstraction.h`, `saveload/saveload.cpp`** — making 1.0.5
  build and run under `-noixemul` libnix, and routing output to stdout because
  the AmigaDOS 3.1 shell cannot redirect stderr.
- **`tgp.cpp`** — `pow()` removed from terrain generation (it is miscompiled in
  this toolchain's soft-float build), plus integer bounds on the coastline
  loops so a bad value can no longer hang the machine instead of producing a
  slightly wrong map.
- **`main_gui.cpp`** — DELETE closes windows in the intro menu too, and the
  OpenTTD logo moves up on lores screens.
- **`intro_gui.cpp`, `gfx_func.h`** — the startup menu moves down on lores
  screens so it no longer overlaps the logo.
- **`strings.cpp`** — screen resolutions are labelled `AGA`, ahead of RTG modes
  joining the same list.
- **`fontcache.{h,cpp}`** — the small interface font used by the lores modes.
  Note the copy in `fontcache.cpp` is inside `#ifdef WITH_FREETYPE` and is dead
  code here; the live one is the inline version in the header.
- **`sound.cpp`** — a three-line hook passing the sound ID to the driver, so
  ambient effects can be capped to two of Paula's four channels and cannot
  drown out gameplay sounds.
- **`music.cpp`, `music_gui.cpp`, `music/amiga_m.*`, `sound/amiga_adpcm.*`** —
  native music. The base music set is synthesised from our own IMA-ADPCM WAV
  tracks (the port ships no copyrighted `.gm` MIDI and does not need any), which
  are streamed from disk and played on Paula channels 2 (right) + 3 (left) with
  gapless double buffering; effects share channels 0+1 while music plays, all
  four when it does not. Shuffle defaults on.
- **`company_cmd.cpp`, `openttd.cpp`, `ai/ai_core.cpp`, `ai_old/oldai.*`** — a
  native C++ AI, a plain-C++ replacement for the Squirrel AI (which cannot
  unwind its `AI_VMSuspend` exception through the Hunk binary). The
  competitor-spawn call in `company_cmd.cpp` is commented out for this release
  (AI opponents disabled at the user's request), but the code is linked in.
- **`table/settings.h`** — autosave defaults to yearly, because autosaving is
  expensive on this hardware.
- **`news_type.h`, `genworld.cpp`, `landscape.cpp`** — news defaults and
  generation-progress logging.
- **`rail_cmd.cpp`** — a bound on the track-laying loop, so a bad drag can no
  longer spin forever.
- **`video/c2p_rect.s`, `video/c2p_glue.s`** — Mikael Kalms' chunky-to-planar
  routines (public domain, see the repository root) and our calling glue.
