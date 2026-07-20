# OpenTTD for AmigaOS 68k (AGA)

A native port of OpenTTD 1.0.5 to classic AmigaOS 3.x on 68k, with its own
video driver written against Intuition and graphics.library. No SDL, no RTG
card required — it runs on plain AGA.

This is early work. It is playable enough to generate a map, build a road and a
station and watch a vehicle drive, but it is not finished software and it is not
a release. Consider it a proof that the thing is possible, plus the notes needed
for someone else to carry it further.

![OpenTTD running on an emulated A4000/040 with AGA](screenshots/newgame.png)

## Why 1.0.5

It is the last comfortable base for this target:

- **C++03** — builds with the bebbo m68k-amigaos GCC 6.5 toolchain. Later
  OpenTTD needs C++11/17/20.
- **8bpp blitter by default** — the game already renders into a chunky
  palette-index buffer, which is exactly the input a chunky-to-planar converter
  wants. Nothing has to be converted twice.
- **All external libraries optional**, so the dependency surface stays small.
- **First legally redistributable version** — OpenGFX and OpenSFX exist from
  1.0.0 onwards. Transport Tycoon Deluxe never shipped on Amiga, so users
  cannot legally be expected to supply the original data files.

1.0.5 also still contains OpenTTD's own Amiga/MorphOS scaffolding; that was only
removed in 2019. What never existed upstream, and what this repository adds, is
a real Amiga video driver.

## How the display works

OpenTTD draws into a chunky 8bpp buffer in Fast RAM. That buffer is handed
straight to the game as `_screen.dst_ptr`, so the blitter needs no adaptation.
Getting it onto an AGA screen is the only Amiga-specific step:

- The screen owns **one contiguous Chip RAM block** for all eight bitplanes,
  built by hand and passed to Intuition with `SA_BitMap`. `AllocBitMap()` does
  not guarantee equally spaced planes, and the c2p requires them.
- Conversion uses **Mikael Kalms' 68040 `c2p_rect`**, which is written for
  exactly this case — dirty-rectangle updates in a windowing system.
- Only changed rectangles are converted. OpenTTD's `DrawDirtyBlocks()` already
  coalesces damage and reports each region separately, so the driver keeps a
  list of them rather than one bounding box. On the title screen that measures
  at 60-70% fewer pixels converted.

Measured on a WinUAE machine calibrated against SysInfo to a real A4000/040/25:

| area | graphics.library `WritePixelArray8` | Kalms `c2p_rect` |
|---|---|---|
| 640x512 full frame | 388 ms | **117 ms** |
| 320x256 | 96 ms | **29.6 ms** |
| 160x64 | 11.4 ms | **3.8 ms** |
| 64x32 | 2.4 ms | **0.7 ms** |

A full-screen repaint is therefore around 8.5 fps on an 040/25 and roughly
14 fps on an 040/40 — but a full repaint almost never happens in normal play.

## State of things

Works: map generation, saving and loading, the menus, the scenario editor,
building track/road/stations, vehicles running, mouse input.

Does not work yet:

- **Keyboard input.** IDCMP delivers no key events to the backdrop window; not
  yet diagnosed.
- **Right-button map scrolling.** Under investigation.
- **Sound.** OpenSFX is recognised as a sound set, but there is no AHI driver,
  so the game runs with `-s null`.
- **Networking** is disabled in this build.
- Memory use is high — roughly 11 MB with the default 4 MB sprite cache and an
  unstripped binary. An 8 MB A1200 will need both trimmed.

## Requirements

- AmigaOS 3.1, 68040 **with FPU** (see BUILDING.md — map generation needs it;
  the game logic itself is integer-only)
- 640x480 or better in 256 colours
- OpenGFX and OpenSFX in `data/`

## Building

See [BUILDING.md](BUILDING.md). Read it before you start — the toolchain has
several traps that produce silently wrong code rather than errors, and each one
cost a day to find.

## Credits

- Written by angree, with Claude (Anthropic) used heavily throughout — for the
  driver code, for the build work, and in particular for the isolation testing
  that tracked down four separate miscompilations in the toolchain.
- OpenTTD, GPL v2 — <https://www.openttd.org/>
- Chunky-to-planar routines by Mikael Kalms, public domain —
  <https://github.com/Kalmalyzer/kalms-c2p>
- OpenGFX and OpenSFX by the OpenTTD community
- bebbo's amiga-gcc toolchain — <https://github.com/bebbo/amiga-gcc>

## Licence

GPL v2, following OpenTTD. Kalms' c2p is public domain and keeps its own terms.
