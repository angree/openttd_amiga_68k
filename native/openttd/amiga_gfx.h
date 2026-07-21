/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Thin C API over the native AmigaOS display and input.
 *
 * DELIBERATELY free of Amiga headers. Including <proto/exec.h> from OpenTTD C++
 * drags in dos/intuition/graphics, whose macros (Insert, Remove, Allocate) and
 * types (struct ViewPort, Point) collide head-on with OpenTTD's own names - that
 * collision is what sank the previous attempt. All Amiga code therefore lives in
 * amiga_gfx.c, compiled as plain C, and the C++ side sees only this file.
 */

#ifndef AMIGA_GFX_H
#define AMIGA_GFX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds handed back by amigagfx_poll(). */
#define AMIGAGFX_EV_NONE      0
#define AMIGAGFX_EV_MOUSEMOVE 1
#define AMIGAGFX_EV_MOUSEDOWN 2
#define AMIGAGFX_EV_MOUSEUP   3
#define AMIGAGFX_EV_KEY       4
#define AMIGAGFX_EV_QUIT      5

/* Button ids for MOUSEDOWN / MOUSEUP. */
#define AMIGAGFX_BUTTON_LEFT  0
#define AMIGAGFX_BUTTON_RIGHT 1

typedef struct {
	int type;    /* AMIGAGFX_EV_* */
	int x, y;    /* pointer position, screen coordinates */
	int code;    /* button id, or raw key code */
} AmigaGfxEvent;

/* Which display backend a screen runs on. Passed to amigagfx_open() and
 * reported back by amigagfx_backend() - which is what actually opened, and may
 * be AGA even when RTG was asked for (see the fallback note on amigagfx_open).
 *
 *   AGA - planar Intuition screen, 8 bitplanes in one contiguous Chip RAM
 *         block, every dirty rectangle pushed through Kalms' chunky-to-planar.
 *   RTG - 8-bit CyberGraphX/Picasso96 screen. The chunky buffer IS the display
 *         format there, so the c2p disappears entirely and a blit becomes a
 *         per-row memcpy into the card's bitmap. No Chip RAM is used at all. */
#define AMIGAGFX_BACKEND_AGA 0
#define AMIGAGFX_BACKEND_RTG 1

/* Is an 8-bit RTG mode of at least w x h available on this machine? Returns 0
 * when cybergraphics.library is missing (a plain AGA Amiga), when the library
 * offers no matching 8-bit mode, or when the "best" mode it returns turns out
 * not to be a Cybergraphics mode at all - BestCModeIDTagList happily falls back
 * to a native chipset mode, which would put us back on the c2p path wearing an
 * RTG label. Safe to call before amigagfx_open(); used to decide which RTG
 * entries to offer in the resolution list at all, so an AGA-only machine sees
 * exactly the list it saw before RTG support existed. */
int amigagfx_rtg_has_mode(int w, int h);

/* AMIGAGFX_BACKEND_* of the screen currently open (AGA when none is). */
int amigagfx_backend(void);

/* Open a w x h, 256-colour screen on the requested backend. Returns 0 on
 * success, non-zero on failure.
 *
 * backend AMIGAGFX_BACKEND_RTG asks for a CyberGraphX/Picasso96 8-bit screen
 * and SILENTLY FALLS BACK to AGA if anything about it fails (no library, no
 * such mode, OpenScreen refused) - one log line says which and why, and the
 * game keeps running. amigagfx_backend() then reports AGA.
 *
 * On AGA w must be a multiple of 32 (the c2p works in 32-pixel columns); on
 * RTG there is no such constraint and the caller must not impose one.
 *
 * show_bar non-zero keeps the SYSTEM screen title bar visible (the real
 * Intuition bar with the depth gadget, so the player can flip to Workbench
 * like any other Amiga program). The bar takes vertical space: the drawable
 * game area is then w x amigagfx_game_height(), which is h minus the bar
 * height Intuition reports for the opened screen - it depends on the font
 * and the mode, so it is never hard-coded. With show_bar 0 the game area is
 * the full w x h, as before. */
int amigagfx_open(int w, int h, int show_bar, int backend);

/* Height in pixels of the drawable game area of the currently open screen:
 * the opened height minus the system title bar when that is visible. This is
 * what _screen.height must be set to. */
int amigagfx_game_height(void);

void amigagfx_close(void);

/* The chunky 8bpp framebuffer, in Fast RAM. This becomes _screen.dst_ptr, so
 * OpenTTD's blitter draws straight into it with no conversion. */
unsigned char *amigagfx_chunky(void);

/* Bytes per row of the chunky buffer (equals the width). */
int amigagfx_pitch(void);

/* rgb points at count*3 bytes. */
void amigagfx_set_palette(const unsigned char *rgb, int first, int count);

/* Push one dirty rectangle to the screen. On AGA x/width are snapped outwards
 * to the 32-pixel grid the c2p requires; on RTG they are used as given, because
 * that granularity is a c2p property and nothing else needs it. Clipping is
 * handled here on both backends. */
void amigagfx_blit(int x, int y, int w, int h);

/* Milliseconds since program start; OpenTTD's main loop needs a ms clock. */
unsigned long amigagfx_millis(void);

/* Pops one pending event. Returns 0 when the queue is empty. */
int amigagfx_poll(AmigaGfxEvent *ev);

/* Move the system pointer to the ABSOLUTE pixel position x,y in the GAME AREA
 * of our screen (the title-bar offset, if any, is added internally) by
 * writing an IECLASS_NEWPOINTERPOS / IESUBCLASS_PIXEL event to input.device
 * (opened lazily, closed again by amigagfx_close). The screen pointer stays
 * internal to amiga_gfx.c. Returns 1 if the event was sent, 0 if it could not
 * be (input.device unavailable) - in that case it is a harmless no-op.
 * NOTE: Intuition will deliver a normal mouse-move IDCMP event for the warp;
 * the caller must expect and suppress it. */
int amigagfx_warp_pointer(int x, int y);

/* Append a line to the log. amigagfx_open() redirects stderr to Work:ottd.log
 * unbuffered, which is the only way to see anything at all: OpenTTD logs to
 * stderr and the AmigaDOS 3.1 shell can only redirect stdout. */
void amigagfx_log(const char *msg);

/* Non-zero re-enables the chatty per-play diagnostics on the C side (the
 * periodic "blit #N" heartbeat). Off by default for a quiet release log; the
 * C++ driver calls this for "-v amiga:verbose". */
void amigagfx_set_verbose(int verbose);

/* Startup memory probe: appends one line to PROGDIR:amiga_mem.log with the
 * label plus AvailMem(MEMF_FAST), AvailMem(MEMF_FAST|MEMF_LARGEST) and
 * AvailMem(MEMF_CHIP) in KB. Total-free vs largest-block matters: with two
 * Fast regions (8 MB + 32 MB Z3) fragmentation can move total-free readings
 * by megabytes with no real change in usage. First call truncates the log;
 * capped at a fixed number of lines so it can never flood. Safe to call at
 * any time, including before amigagfx_open(). Lives here because AvailMem
 * needs Amiga headers, which OpenTTD C++ must never include. */
void AmigaMemProbe(const char *label);

/* Show the startup splash image (ASPL file, see build/make-splash.py) on the
 * already-open screen: fade in ~0.5 s, hold 2.5 s, fade out ~0.5 s. The image
 * is converted chunky-to-planar exactly once; the fade animates only the
 * palette registers, so it is cheap even on a 68030. Draws at 1x on screens
 * below 400 lines, 2x nearest-neighbour otherwise. On any problem (missing
 * file, bad magic, does not fit) it logs one line and returns without drawing.
 * Leaves the chunky buffer cleared to index 0 and the palette black - the
 * caller must restore the game palette afterwards. */
void amigagfx_splash(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_GFX_H */
