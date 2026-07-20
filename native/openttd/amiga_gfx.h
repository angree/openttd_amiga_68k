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

/* Open a w x h, 256-colour AGA screen. Returns 0 on success, non-zero on
 * failure. w must be a multiple of 32 (the c2p works in 32-pixel columns). */
int amigagfx_open(int w, int h);

void amigagfx_close(void);

/* The chunky 8bpp framebuffer, in Fast RAM. This becomes _screen.dst_ptr, so
 * OpenTTD's blitter draws straight into it with no conversion. */
unsigned char *amigagfx_chunky(void);

/* Bytes per row of the chunky buffer (equals the width). */
int amigagfx_pitch(void);

/* rgb points at count*3 bytes. */
void amigagfx_set_palette(const unsigned char *rgb, int first, int count);

/* Push one dirty rectangle to the screen. x/width are snapped outwards to the
 * 32-pixel grid the c2p requires; clipping is handled here. */
void amigagfx_blit(int x, int y, int w, int h);

/* Milliseconds since program start; OpenTTD's main loop needs a ms clock. */
unsigned long amigagfx_millis(void);

/* Pops one pending event. Returns 0 when the queue is empty. */
int amigagfx_poll(AmigaGfxEvent *ev);

/* Move the system pointer to the ABSOLUTE pixel position x,y on OUR screen by
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
