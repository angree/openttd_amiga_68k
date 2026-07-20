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

/* Append a line to the log. amigagfx_open() redirects stderr to Work:ottd.log
 * unbuffered, which is the only way to see anything at all: OpenTTD logs to
 * stderr and the AmigaDOS 3.1 shell can only redirect stdout. */
void amigagfx_log(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_GFX_H */
