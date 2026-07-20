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
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
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

/* Chatty per-play diagnostics (currently only the periodic "blit #N" line) are
 * off by default; the C++ driver switches them on for "-v amiga:verbose". */
static int g_verbose;

void amigagfx_set_verbose(int verbose)
{
	g_verbose = verbose;
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
static ULONG g_want_modeid;
static int   g_used_fallback;

/* input.device, opened lazily and only for IND_WRITEEVENT: Intuition has no
 * "warp the pointer" call, but feeding an IECLASS_NEWPOINTERPOS event with
 * IESUBCLASS_PIXEL (screen + pixel coordinates) into the input stream moves
 * the system pointer to an absolute position. Needed for right-drag map
 * scrolling, where the pointer must not pile up at the screen edge.
 * g_input_state: 0 = not tried yet, 1 = open, -1 = failed (stay a no-op). */
static struct MsgPort  *g_input_mp;
static struct IOStdReq *g_input_io;
static int              g_input_state;

static int input_device_open(void)
{
	if (g_input_state != 0) return g_input_state > 0;
	g_input_state = -1;

	g_input_mp = CreateMsgPort();
	if (g_input_mp == NULL) {
		amigagfx_log("input.device: CreateMsgPort FAILED - pointer warp disabled");
		return 0;
	}

	g_input_io = (struct IOStdReq *)CreateIORequest(g_input_mp, sizeof(struct IOStdReq));
	if (g_input_io == NULL) {
		DeleteMsgPort(g_input_mp); g_input_mp = NULL;
		amigagfx_log("input.device: CreateIORequest FAILED - pointer warp disabled");
		return 0;
	}

	if (OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)g_input_io, 0) != 0) {
		DeleteIORequest(g_input_io); g_input_io = NULL;
		DeleteMsgPort(g_input_mp);   g_input_mp = NULL;
		amigagfx_log("input.device: OpenDevice FAILED - pointer warp disabled");
		return 0;
	}

	amigagfx_log("input.device open - pointer warp ready (NEWPOINTERPOS/PIXEL)");
	g_input_state = 1;
	return 1;
}

static void input_device_close(void)
{
	if (g_input_state > 0) CloseDevice((struct IORequest *)g_input_io);
	if (g_input_io != NULL) { DeleteIORequest(g_input_io); g_input_io = NULL; }
	if (g_input_mp != NULL) { DeleteMsgPort(g_input_mp);   g_input_mp = NULL; }
	g_input_state = 0;
}

int amigagfx_warp_pointer(int x, int y)
{
	struct InputEvent ie;
	struct IEPointerPixel pp;
	BYTE err;

	if (g_screen == NULL) return 0;
	if (!input_device_open()) return 0;

	/* IECLASS_POINTERPOS did NOT work here: its ie_X/ie_Y are in the input
	 * device's own coordinate space, not screen pixels, and on our
	 * hires-interlaced screens the two differ per axis - the pointer never
	 * visibly moved. IECLASS_NEWPOINTERPOS (V36, so any OS 2.0+) with
	 * IESUBCLASS_PIXEL instead takes an explicit target screen plus true
	 * pixel coordinates in that screen, which is unambiguous in every mode. */
	pp.iepp_Screen     = g_screen;
	pp.iepp_Position.X = (WORD)x;
	pp.iepp_Position.Y = (WORD)y;

	memset(&ie, 0, sizeof(ie));        /* zeroes ie_NextEvent and ie_TimeStamp */
	ie.ie_Class        = IECLASS_NEWPOINTERPOS;
	ie.ie_SubClass     = IESUBCLASS_PIXEL;
	ie.ie_Code         = IECODE_NOBUTTON;
	ie.ie_Qualifier    = 0;            /* absolute: no IEQUALIFIER_RELATIVEMOUSE */
	ie.ie_EventAddress = &pp;          /* pp lives across DoIO - it is synchronous */

	g_input_io->io_Command = IND_WRITEEVENT;
	g_input_io->io_Data    = &ie;
	g_input_io->io_Length  = sizeof(ie);
	err = DoIO((struct IORequest *)g_input_io);

	{
		static int warps, fails;
		if ((err == 0 && warps < 6) || (err != 0 && fails < 4)) {
			char b[96];
			snprintf(b, sizeof(b), "warp sent: pixel %d,%d on %dx%d screen, DoIO rc=%d",
			         x, y, g_width, g_height, (int)err);
			amigagfx_log(b);
			if (err == 0) warps++; else fails++;
		}
	}
	return err == 0;
}

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

	/* Pick the display mode from the size actually asked for, instead of always
	 * forcing hires-interlaced. 320-wide means lores, and anything at or below
	 * 256 lines fits a PAL frame without interlace - which also means no
	 * flicker, the main reason to want a lores mode in the first place. */
	{
		ULONG modeid = PAL_MONITOR_ID;
		modeid |= (w > 400) ? HIRES_KEY : LORES_KEY;
		if (h > 300) modeid |= 0x0004;          /* LACE bit */
		g_want_modeid = modeid;

		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)DEPTH,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     (ULONG)TRUE,
		                          SA_ShowTitle, (ULONG)FALSE,
		                          SA_DisplayID, modeid,
		                          TAG_END);
	}
	if (g_screen == NULL) {
		/* Fallback: the system picks. NOTE this drops our mode entirely, so a
		 * lores request silently becomes whatever Workbench runs - usually hires. */
		g_used_fallback = 1;
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
	{
		/* Report the mode Intuition ACTUALLY granted, not the one we asked for.
		 * It substitutes silently when a mode is unavailable, and a lores-sized
		 * screen running in a hires mode looks identical in a log that only
		 * prints the pixel dimensions. */
		ULONG got = GetVPModeID(&g_screen->ViewPort);
		fprintf(stdout, "amiga: screen open %dx%d depth 8, bpr %d\n", w, h, g_bpr);
		fprintf(stdout, "amiga: modeid wanted $%08lx got $%08lx  %s%s  [%s]\n",
		        (unsigned long)g_want_modeid, (unsigned long)got,
		        (got & HIRES_KEY) ? "HIRES " : "LORES ",
		        (got & 0x0004) ? "INTERLACED" : "non-interlaced",
		        g_used_fallback ? "SYSTEM FALLBACK - our mode was refused"
		                        : "our mode accepted");
		fflush(stdout);
	}

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
	input_device_close();
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

/* ---- Startup splash ------------------------------------------------------
 *
 * File format ("ASPL", written by build/make-splash.py), all fields big-endian
 * which on this 68k is simply native order - but they are still assembled
 * byte-by-byte so the loader does not depend on struct layout or alignment:
 *
 *   offset 0  : magic    4 bytes, ASCII "ASPL"
 *   offset 4  : uint16   width
 *   offset 6  : uint16   height
 *   offset 8  : uint16   ncolours (<= 256)
 *   offset 10 : uint16   reserved (0)
 *   offset 12 : palette  ncolours * 3 bytes, R,G,B
 *   then      : pixels   width * height bytes of palette indices
 *
 * Index 0 is pure black and fills the screen around the image, so the whole
 * screen fades as one. The fade NEVER re-runs the c2p or touches the chunky
 * buffer: the image is converted once, then only the colour registers are
 * rewritten with the stored RGB triples scaled by a 0..256 fixed-point
 * factor. A fade step is 256 palette writes, not a frame conversion. */

#define SPLASH_FADE_MS 500UL
#define SPLASH_HOLD_TICKS 125   /* Delay() ticks of 20 ms -> 2.5 s */

/* One palette write of the splash palette scaled by factor 0..256. */
static void splash_palette_step(const UBYTE *pal, int ncol, long factor)
{
	UBYTE rgb[256 * 3];
	int i;

	for (i = 0; i < ncol * 3; i++) {
		rgb[i] = (UBYTE)(((long)pal[i] * factor) >> 8);
	}
	amigagfx_set_palette(rgb, 0, ncol);
}

/* Ramp the palette factor from 'from' to 'to' over dur_ms, paced by the
 * DateStamp millisecond clock (20 ms granularity) and Delay(1). */
static void splash_fade(const UBYTE *pal, int ncol, long from, long to, ULONG dur_ms)
{
	ULONG t0 = amigagfx_millis();

	for (;;) {
		ULONG el = amigagfx_millis() - t0;
		if (el >= dur_ms) break;
		splash_palette_step(pal, ncol, from + ((to - from) * (long)el) / (long)dur_ms);
		Delay(1);
	}
	splash_palette_step(pal, ncol, to);   /* land exactly on the end value */
}

void amigagfx_splash(const char *path)
{
	FILE *f = NULL;
	UBYTE *pix = NULL;
	UBYTE hdr[12];
	UBYTE pal[256 * 3];
	int w, h, ncol, scale, dx, dy, x, y;
	char msg[128];

	if (g_screen == NULL || g_chunky == NULL) return;

	f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(msg, sizeof(msg), "splash: %s not found - skipped", path);
		amigagfx_log(msg);
		return;
	}

	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
	    hdr[0] != 'A' || hdr[1] != 'S' || hdr[2] != 'P' || hdr[3] != 'L') {
		amigagfx_log("splash: bad or truncated header - skipped");
		fclose(f);
		return;
	}

	w    = ((int)hdr[4] << 8) | hdr[5];
	h    = ((int)hdr[6] << 8) | hdr[7];
	ncol = ((int)hdr[8] << 8) | hdr[9];

	/* 2x nearest-neighbour on tall (hires) screens, 1x on lores. */
	scale = (g_height >= 400) ? 2 : 1;

	if (w <= 0 || h <= 0 || ncol <= 0 || ncol > 256 ||
	    w * scale > g_width || h * scale > g_height) {
		snprintf(msg, sizeof(msg), "splash: %dx%d ncol %d does not fit %dx%d @%dx - skipped",
		         w, h, ncol, g_width, g_height, scale);
		amigagfx_log(msg);
		fclose(f);
		return;
	}

	pix = (UBYTE *)AllocVec((ULONG)w * h, MEMF_ANY);
	if (pix == NULL) {
		amigagfx_log("splash: no memory for pixels - skipped");
		fclose(f);
		return;
	}

	if (fread(pal, 1, (size_t)ncol * 3, f) != (size_t)ncol * 3 ||
	    fread(pix, 1, (size_t)w * h, f) != (size_t)w * h) {
		amigagfx_log("splash: file shorter than header claims - skipped");
		FreeVec(pix);
		fclose(f);
		return;
	}
	fclose(f);
	f = NULL;

	/* Black the palette out BEFORE the image reaches the screen, so the fade
	 * starts from darkness instead of flashing the game palette. */
	splash_palette_step(pal, ncol, 0);

	/* Compose once: black border (index 0), image centred. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	dx = (g_width  - w * scale) / 2;
	dy = (g_height - h * scale) / 2;
	for (y = 0; y < h; y++) {
		const UBYTE *src = pix + (ULONG)y * w;
		UBYTE *dst = g_chunky + (ULONG)(dy + y * scale) * g_width + dx;
		if (scale == 1) {
			memcpy(dst, src, (size_t)w);
		} else {
			UBYTE *d = dst;
			for (x = 0; x < w; x++) { UBYTE c = src[x]; *d++ = c; *d++ = c; }
			memcpy(dst + g_width, dst, (size_t)w * 2);   /* double the row */
		}
	}
	FreeVec(pix);
	pix = NULL;

	/* The ONE chunky-to-planar conversion of the splash. */
	amigagfx_blit(0, 0, g_width, g_height);

	snprintf(msg, sizeof(msg), "splash: %dx%d ncol %d at %d,%d scale %dx", w, h, ncol, dx, dy, scale);
	amigagfx_log(msg);

	/* Fade in, hold, fade out - palette-only from here on. */
	splash_fade(pal, ncol, 0, 256, SPLASH_FADE_MS);
	Delay(SPLASH_HOLD_TICKS);
	splash_fade(pal, ncol, 256, 0, SPLASH_FADE_MS);

	/* Leave the screen genuinely black: clear the chunky buffer and convert
	 * once more, so restoring the game palette cannot flash the image back. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	amigagfx_blit(0, 0, g_width, g_height);
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

	/* The FIRST blit is logged always - it is the one-shot proof that the main
	 * loop reached the screen. The every-200th heartbeat costs I/O for the
	 * whole session, so it now needs "-v amiga:verbose". */
	g_blits++;
	if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
		fprintf(stdout, "amiga: blit #%lu  %dx%d at %d,%d\n",
		        g_blits, (int)args.w, (int)args.h, (int)args.x, (int)args.y);
		fflush(stdout);
	}
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
			/* Pass EVERY raw code through, releases included (bit 7 set): the
			 * C++ side needs them to track shift and control. ESC is no longer
			 * swallowed as "quit" either - the game wants it for closing
			 * windows, and quitting belongs in the menu. */
			ev->type = AMIGAGFX_EV_KEY;
			ev->code = (int)code;
			break;
		default:
			break;
		}
	}
	return 1;
}
