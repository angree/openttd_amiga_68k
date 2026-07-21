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
cp native/openttd/amiga_v.cpp  native/openttd/amiga_v.h    src/video/
cp native/openttd/amiga_gfx.c  native/openttd/amiga_gfx.h  src/video/
cp native/openttd/amiga_s.cpp  native/openttd/amiga_s.h    src/sound/
cp native/openttd/amiga_audio.c native/openttd/amiga_audio.h src/sound/
cp native/openttd/fp_conv.c    src/
cp native/c2p_rect.s native/c2p_glue.s src/video/
```

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
- **`table/settings.h`** — autosave defaults to yearly, because autosaving is
  expensive on this hardware.
- **`news_type.h`, `genworld.cpp`, `landscape.cpp`** — news defaults and
  generation-progress logging.
- **`rail_cmd.cpp`** — a bound on the track-laying loop, so a bad drag can no
  longer spin forever.
- **`video/c2p_rect.s`, `video/c2p_glue.s`** — Mikael Kalms' chunky-to-planar
  routines (public domain, see the repository root) and our calling glue.
