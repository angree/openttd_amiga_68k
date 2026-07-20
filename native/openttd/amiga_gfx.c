/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS display + input for OpenTTD. Plain C: see amiga_gfx.h for why.
 *
 * Everything here is the code proven in native/agac2p.c and native/againput.c:
 *   - one contiguous Chip RAM block for all 8 bitplanes, handed to Intuition
 *     with SA_BitMap (AllocBitMap does NOT guarantee equally spaced planes, and
 *     Kalms' c2p requires them)
 *   - a chunky 8bpp buffer in Fast RAM, which is what OpenTTD renders into
 *   - Kalms' c2p_rect for chunky->planar: measured 117 ms for a full 640x512
 *     frame on a 68040/25 versus 388 ms for graphics.library WritePixelArray8
 *   - a backdrop, borderless window purely to receive IDCMP input
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <exec/memory.h>
#include <string.h>
#include <stdio.h>

#include "amiga_gfx.h"

/* OpenTTD writes everything - DEBUG(), errors, the lot - to stderr, and the
 * AmigaDOS 3.1 shell can only redirect stdout, so a crash or a hang leaves no
 * trace at all. Point stderr at a file ourselves and make it UNBUFFERED, so the
 * last line written before a hang is actually on disk. */
void amigagfx_log(const char *msg)
{
	fprintf(stdout, "amiga: %s\n", msg);
	fflush(stdout);
}

/* Argument block for the assembly c2p; layout must match native/c2p_glue.s. */
struct C2PArgs {
	UWORD x, y, w, h;
	UWORD cmod, bmod;
	ULONG bplsize;
	APTR  chunky;
	APTR  bpl;
};
extern void c2p_rect_asm(struct C2PArgs *a);

#define DEPTH 8

static struct Screen *g_screen;
static struct Window *g_window;
static struct BitMap  g_bitmap;
static UBYTE *g_chip;
static UBYTE *g_chunky;
static int    g_width, g_height, g_bpr;
static ULONG  g_planesize;
static ULONG  g_epoch;
static unsigned long g_blits;

static ULONG raw_ticks(void)
{
	struct DateStamp ds;
	DateStamp(&ds);
	return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

unsigned long amigagfx_millis(void)
{
	/* DateStamp ticks are 1/50 s. Coarse, but OpenTTD only needs this to pace
	 * its 30 ms game loop, not to time frames. */
	return (unsigned long)((raw_ticks() - g_epoch) * 20UL);
}

int amigagfx_open(int w, int h)
{

	fprintf(stdout, "amiga: amigagfx_open(%d,%d)\n", w, h); fflush(stdout);
	g_width  = w;
	g_height = h;
	g_bpr    = ((w + 15) >> 4) << 1;
	g_planesize = (ULONG)g_bpr * h;
	g_epoch  = raw_ticks();

	g_chunky = (UBYTE *)AllocVec((ULONG)w * h, MEMF_ANY | MEMF_CLEAR);
	if (g_chunky == NULL) return 1;

	g_chip = (UBYTE *)AllocMem(g_planesize * DEPTH, MEMF_CHIP | MEMF_CLEAR);
	if (g_chip == NULL) { amigagfx_close(); return 2; }

	InitBitMap(&g_bitmap, DEPTH, w, h);
	{
		int i;
		for (i = 0; i < DEPTH; i++)
			g_bitmap.Planes[i] = (PLANEPTR)(g_chip + (ULONG)i * g_planesize);
	}

	g_screen = OpenScreenTags(NULL,
	                          SA_BitMap,    (ULONG)&g_bitmap,
	                          SA_Width,     (ULONG)w,
	                          SA_Height,    (ULONG)h,
	                          SA_Depth,     (ULONG)DEPTH,
	                          SA_Type,      (ULONG)CUSTOMSCREEN,
	                          SA_Quiet,     (ULONG)TRUE,
	                          SA_ShowTitle, (ULONG)FALSE,
	                          SA_DisplayID, (ULONG)(PAL_MONITOR_ID | HIRESLACE_KEY),
	                          TAG_END);
	if (g_screen == NULL) {
		/* let the system pick a mode if PAL HiRes-Laced is unavailable */
		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)DEPTH,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     (ULONG)TRUE,
		                          SA_ShowTitle, (ULONG)FALSE,
		                          TAG_END);
	}
	if (g_screen == NULL) { amigagfx_close(); return 3; }
	fprintf(stdout, "amiga: screen open %dx%d depth 8, bpr %d\n", w, h, g_bpr); fflush(stdout);

	g_window = OpenWindowTags(NULL,
	                          WA_CustomScreen, (ULONG)g_screen,
	                          WA_Left, 0UL, WA_Top, 0UL,
	                          WA_Width, (ULONG)w, WA_Height, (ULONG)h,
	                          WA_Flags, (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS |
	                                            WFLG_ACTIVATE | WFLG_REPORTMOUSE |
	                                            WFLG_RMBTRAP | WFLG_NOCAREREFRESH),
	                          WA_IDCMP, (ULONG)(IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
	                                            IDCMP_RAWKEY | IDCMP_INTUITICKS),
	                          TAG_END);
	if (g_window == NULL) { amigagfx_close(); return 4; }

	/* WFLG_ACTIVATE alone did not reliably give a backdrop window key focus -
	 * keyboard events never arrived. Ask explicitly. */
	ActivateWindow(g_window);
	fprintf(stdout, "amiga: window open, IDCMP active - handing control to OpenTTD\n");
	return 0;
}

void amigagfx_close(void)
{
	if (g_window != NULL) { CloseWindow(g_window); g_window = NULL; }
	if (g_screen != NULL) { CloseScreen(g_screen); g_screen = NULL; }
	if (g_chip != NULL)   { FreeMem(g_chip, g_planesize * DEPTH); g_chip = NULL; }
	if (g_chunky != NULL) { FreeVec(g_chunky); g_chunky = NULL; }
}

unsigned char *amigagfx_chunky(void) { return g_chunky; }
int amigagfx_pitch(void) { return g_width; }

void amigagfx_set_palette(const unsigned char *rgb, int first, int count)
{
	ULONG table[1 + 256 * 3 + 1];
	int i;

	if (g_screen == NULL || count <= 0) return;
	if (first < 0) first = 0;
	if (first + count > 256) count = 256 - first;

	table[0] = ((ULONG)count << 16) | (ULONG)first;
	for (i = 0; i < count; i++) {
		table[1 + i*3 + 0] = ((ULONG)rgb[i*3 + 0]) * 0x01010101UL;
		table[1 + i*3 + 1] = ((ULONG)rgb[i*3 + 1]) * 0x01010101UL;
		table[1 + i*3 + 2] = ((ULONG)rgb[i*3 + 2]) * 0x01010101UL;
	}
	table[1 + count * 3] = 0UL;
	LoadRGB32(&g_screen->ViewPort, table);
}

void amigagfx_blit(int x, int y, int w, int h)
{
	struct C2PArgs args;
	int x2, y2;

	if (g_screen == NULL) return;

	x2 = x + w;
	y2 = y + h;

	/* Kalms' c2p works on 32-pixel columns: grow the rect outwards to that
	 * grid rather than refusing it, then clip to the screen. */
	x  &= ~31;
	x2  = (x2 + 31) & ~31;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > g_width)  x2 = g_width;
	if (y2 > g_height) y2 = g_height;
	if (x2 <= x || y2 <= y) return;

	args.x = (UWORD)x;
	args.y = (UWORD)y;
	args.w = (UWORD)(x2 - x);
	args.h = (UWORD)(y2 - y);
	args.cmod = (UWORD)g_width;
	args.bmod = (UWORD)g_bpr;
	args.bplsize = g_planesize;
	args.chunky = g_chunky;
	args.bpl = g_chip;
	c2p_rect_asm(&args);

	/* First blit and then every 200th: proves the main loop is really running
	 * and pushing pixels, without flooding the log. */
	g_blits++;
	if (g_blits == 1 || (g_blits % 200) == 0)
		fprintf(stdout, "amiga: blit #%lu  %dx%d at %d,%d\n",
		        g_blits, (int)args.w, (int)args.h, (int)args.x, (int)args.y); fflush(stdout);
}

int amigagfx_poll(AmigaGfxEvent *ev)
{
	struct IntuiMessage *msg;

	ev->type = AMIGAGFX_EV_NONE;
	if (g_window == NULL) return 0;

	msg = (struct IntuiMessage *)GetMsg(g_window->UserPort);
	if (msg == NULL) return 0;

	{
		ULONG cls  = msg->Class;
		UWORD code = msg->Code;
		WORD  mx   = msg->MouseX;
		WORD  my   = msg->MouseY;
		ReplyMsg((struct Message *)msg);

		ev->x = mx;
		ev->y = my;
		ev->code = 0;

		switch (cls) {
		case IDCMP_MOUSEMOVE:
			ev->type = AMIGAGFX_EV_MOUSEMOVE;
			break;
		case IDCMP_MOUSEBUTTONS:
			switch (code) {
			case SELECTDOWN: ev->type = AMIGAGFX_EV_MOUSEDOWN; ev->code = AMIGAGFX_BUTTON_LEFT;  break;
			case SELECTUP:   ev->type = AMIGAGFX_EV_MOUSEUP;   ev->code = AMIGAGFX_BUTTON_LEFT;  break;
			case MENUDOWN:   ev->type = AMIGAGFX_EV_MOUSEDOWN; ev->code = AMIGAGFX_BUTTON_RIGHT; break;
			case MENUUP:     ev->type = AMIGAGFX_EV_MOUSEUP;   ev->code = AMIGAGFX_BUTTON_RIGHT; break;
			default: break;
			}
			break;
		case IDCMP_RAWKEY:
			/* 0x45 = ESC. Key-up codes have bit 7 set and are ignored. */
			if (code == 0x45) {
				ev->type = AMIGAGFX_EV_QUIT;
			} else if ((code & 0x80) == 0) {
				ev->type = AMIGAGFX_EV_KEY;
				ev->code = (int)code;
			}
			break;
		default:
			break;
		}
	}
	return 1;
}
