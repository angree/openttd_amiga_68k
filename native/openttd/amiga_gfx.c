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
 *
 * Since 2026-07-21 there is a SECOND display backend here: CyberGraphX/RTG.
 * On an 8-bit RTG screen the chunky buffer is already the display format, so
 * the whole chunky-to-planar step - by far the most expensive thing this file
 * does, 117 ms for a full 640x512 frame on an 040/25 - simply disappears and a
 * dirty rectangle becomes a per-row memcpy into the card's bitmap. The two
 * backends share everything above the blit (chunky buffer, dirty rectangles,
 * Intuition input window, palette, splash) and differ only in how a screen is
 * opened and how a rectangle reaches it.
 *
 * cybergraphics.library is targeted rather than the Picasso96-specific API,
 * because P96 ships a CGX-compatible layer: one implementation then serves both
 * P96 and genuine CyberGraphX users. The library is opened at runtime and
 * everything falls back to AGA if it is not there.
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

/* cybergraphics.library is not part of the bebbo NDK, so the official CGX
 * developer headers are vendored under cgx-include/ and reached with an -I on
 * this file's (hand-written) compile line. The inline/ header there is the
 * repaired one: the 1995 FD2Inline original lists d0 both as the "=r" output
 * and in the clobber list and declares "register _res" with no type at all,
 * which GCC 6.5 either rejects or miscompiles. */
#include <proto/cybergraphics.h>

#include "amiga_gfx.h"

/* Declared extern by <proto/cybergraphics.h>; ours to define and to fill. */
struct Library *CyberGfxBase;

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

/* ---- startup memory probe -----------------------------------------------
 * Which startup phase takes ~5 MB of Fast RAM and which gives it back?
 * One line per call: total free Fast, largest free Fast block, free Chip -
 * all in KB. Total vs largest matters because the machine has two Fast
 * regions (8 MB motherboard + 32 MB Z3): an allocation that lands in the
 * other region, or fragmentation, moves total-free by megabytes while
 * largest-block tells the real story. The log is only readable from the
 * host after the game exits, so one run must be conclusive: every line is
 * numbered and labelled, the first call truncates the file, and a hard cap
 * keeps it from flooding no matter how often a phase repeats. */
#define AMIGA_MEM_LOG      "PROGDIR:amiga_mem.log"
#define AMIGA_MEM_MAXLINES 100

void AmigaMemProbe(const char *label)
{
	static int mem_lines = 0;
	ULONG fast, largest, chip;
	FILE *f;

	if (mem_lines >= AMIGA_MEM_MAXLINES) return;

	/* Forbid so the three readings are one consistent snapshot. */
	Forbid();
	fast    = AvailMem(MEMF_FAST);
	largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
	chip    = AvailMem(MEMF_CHIP);
	Permit();

	f = fopen(AMIGA_MEM_LOG, mem_lines == 0 ? "w" : "a");
	if (f == NULL) return;
	mem_lines++;
	fprintf(f, "M%03d %-26s fast=%luK largest=%luK chip=%luK\n",
	        mem_lines, label,
	        (unsigned long)(fast >> 10),
	        (unsigned long)(largest >> 10),
	        (unsigned long)(chip >> 10));
	fclose(f);
}

/* Argument blocks for the two assembly c2p routines; both layouts must match
 * native/c2p_glue.s exactly. See that file for why there are two. */
struct C2PArgs {
	UWORD x, y, w, h;
	UWORD cmod, bmod;
	ULONG bplsize;
	APTR  chunky;
	APTR  bpl;
};
extern void c2p_rect_asm(struct C2PArgs *a);

struct C2P6Args {
	UWORD chunkyx, chunkyy;
	UWORD offsx, offsy;
	APTR  chunky;
	APTR  bitmap;
};
extern void c2p6_bm_asm(struct C2P6Args *a);

/* Bitplanes per backend. EHB's six are the whole point of the mode: a quarter
 * less Chip RAM for the display and a quarter less work in the c2p, on a chipset
 * feature every Amiga back to the A1000 has. */
#define DEPTH_AGA 8
#define DEPTH_EHB 6

/* Rows of scratch for the EHB c2p, which needs contiguous chunky input (see
 * amigagfx_blit). A band rather than a full-screen copy: 64 rows of the widest
 * EHB mode is 22 KB, where a full-screen shadow of the chunky buffer would be
 * 160 KB standing idle for the whole session. The band is walked down the
 * rectangle, so height is never a limit - only how much is copied at once. */
#define EHB_SCRATCH_ROWS 64

static struct Screen *g_screen;
static struct Window *g_window;
static struct BitMap  g_bitmap;
static UBYTE *g_chip;
static UBYTE *g_chunky;
static UBYTE *g_ehb_scratch;              /* NULL unless an EHB screen is open */
static int    g_ehb_scratch_rows;
static UBYTE  g_ehb_pal[64 * 3];          /* set by amigagfx_set_ehb_palette */
static int    g_ehb_pal_valid;
static int    g_depth;                    /* bitplanes of the open planar screen */
static int    g_width, g_height, g_bpr;   /* g_height = GAME AREA height */
static int    g_yoff;                     /* first game-area line: 0, or the
                                           * system title bar height when the
                                           * bar is left visible */
static ULONG  g_planesize;
static ULONG  g_epoch;
static unsigned long g_blits;
static ULONG g_want_modeid;
static int   g_used_fallback;

/* ---- RTG (CyberGraphX / Picasso96) state --------------------------------
 *
 * g_backend says which of the two paths the CURRENTLY OPEN screen uses. It is
 * the single switch every backend-dependent decision below reads, so nothing
 * has to re-derive "am I on RTG" from the width or the mode id.
 *
 * How a rectangle reaches an 8-bit RTG screen - the two candidates, and why
 * this is the order:
 *
 *   RTG_METHOD_LOCK (preferred): LockBitMapTagList() hands back the base
 *     address and bytes-per-row of the screen's own bitmap, which on an 8-bit
 *     LUT8 screen holds exactly the bytes our chunky buffer holds. The blit is
 *     then a memcpy per row into card memory - no conversion and no
 *     intermediate copy, which is the entire reason to want RTG.
 *
 *   RTG_METHOD_WLUT (fallback): WriteLUTPixelArray() with a CTABFMT_XRGB8
 *     colour table. Always correct, but it goes through the library per
 *     rectangle and, because it is specified in terms of colours rather than
 *     pen numbers, it may remap every pixel instead of copying it. Used only if
 *     the lock is refused or reports something other than a LUT8 8-bit bitmap.
 *
 * The choice is made by ACTUALLY TAKING the lock once when the screen opens
 * (probe_rtg_lock) rather than by trusting the mode we asked for, and a later
 * failure demotes to WLUT permanently for that screen.
 *
 * LOCKING DISCIPLINE - getting this wrong hangs the machine, so it is stated
 * once and obeyed everywhere:
 *   1. Between LockBitMapTagList() and UnLockBitMap() this file calls NOTHING:
 *      no OS function, no logging, no Wait, no allocation. Only memcpy over
 *      memory whose bounds were computed BEFORE the lock was taken.
 *   2. Every path out of a locked region passes through exactly one
 *      UnLockBitMap(). The validity check inside the lock does not return
 *      early; it sets a flag, falls out, unlocks, and only then acts on it.
 *   3. The base address and bytes-per-row are valid ONLY inside the lock that
 *      produced them. They are re-read on every single lock and never cached
 *      across one - RTG display memory can move between locks.
 *   4. The lock is held for one rectangle at a time, never across the event
 *      loop and never across a frame.
 */
#define RTG_METHOD_LOCK 0
#define RTG_METHOD_WLUT 1

static int   g_backend;        /* AMIGAGFX_BACKEND_* of the open screen */
static int   g_rtg_method;
static int   g_rtg_demoted;    /* log the LOCK->WLUT demotion once */
static ULONG g_ctable[256];    /* CTABFMT_XRGB8 mirror of the palette */

static UBYTE g_screen_title[] = "OpenTTD 68K";

/* Intuition's default screen pens (DetailPen 0, BlockPen 1) index whatever
 * palette the GAME loads - and in the TTD *Windows* palette (OpenGFX's
 * default) indices 1..9 are magenta (212,0,212): that was the pink bar.
 * The indices below hold the SAME colour in both TTD palettes (DOS and
 * Windows, src/table/palettes.h) and lie outside the animated range
 * 217..254, so they survive UpdatePalette() and any base-set choice:
 *   0  = black          (0,0,0)
 *   15 = white          (252,252,252)
 *   17 = dark blue-grey (68,76,92)   - OpenTTD's own chrome family
 *   19 = mid blue-grey  (108,116,132)
 * Result: dark neutral bar, white legible title, black trim line. */
static UWORD g_screen_pens[] = {
	15,         /* DETAILPEN        - bar text, old (1.3) look   */
	17,         /* BLOCKPEN         - bar fill, old (1.3) look   */
	15,         /* TEXTPEN          - text on BACKGROUNDPEN      */
	15,         /* SHINEPEN         - bevel light edge           */
	0,          /* SHADOWPEN        - bevel dark edge            */
	19,         /* FILLPEN          - selected gadget fill       */
	15,         /* FILLTEXTPEN                                   */
	17,         /* BACKGROUNDPEN                                 */
	15,         /* HIGHLIGHTTEXTPEN                              */
	15,         /* BARDETAILPEN     - screen title text (3.x)    */
	17,         /* BARBLOCKPEN     - screen title bar fill (3.x) */
	0,          /* BARTRIMPEN       - line under the bar         */
	(UWORD)~0
};

int amigagfx_backend(void) { return g_backend; }

/* cybergraphics.library, opened on demand. V41 is the first version with
 * LockBitMapTagList and WriteLUTPixelArray, and it is also what Picasso96's
 * compatibility layer reports, so it is the right floor for both. */
static int cgx_open(void)
{
	if (CyberGfxBase != NULL) return 1;
	CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 41L);
	if (CyberGfxBase == NULL) {
		amigagfx_log("cybergraphics.library v41 not available - RTG modes disabled");
		return 0;
	}
	return 1;
}

static void cgx_close(void)
{
	if (CyberGfxBase != NULL) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
}

/* Ask CGX for the best 8-bit mode of at least w x h, then verify it really is
 * one. BestCModeIDTagList never fails outright: when nothing matches it returns
 * a NATIVE chipset mode id, which would silently put an "RTG" screen back on
 * the planar path with no c2p behind it - garbage on screen. So the result is
 * checked three ways (is it a Cybergraphics id, is it 8 bits deep, is it one
 * byte per pixel) before it is believed. Returns INVALID_ID if not usable. */
static ULONG rtg_best_mode(int w, int h)
{
	ULONG id;
	struct TagItem tags[4];

	if (!cgx_open()) return (ULONG)INVALID_ID;

	tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)w;
	tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)h;
	tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = 8UL;
	tags[3].ti_Tag = TAG_END;                 tags[3].ti_Data = 0UL;

	id = BestCModeIDTagList(tags);

	if (id == (ULONG)INVALID_ID) return (ULONG)INVALID_ID;
	if (!IsCyberModeID(id))      return (ULONG)INVALID_ID;
	if (GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8UL) return (ULONG)INVALID_ID;
	if (GetCyberIDAttr(CYBRIDATTR_BPPIX, id) != 1UL) return (ULONG)INVALID_ID;

	return id;
}

int amigagfx_rtg_has_mode(int w, int h)
{
	ULONG id = rtg_best_mode(w, h);

	/* The probe runs while building the resolution list, long before any
	 * screen exists. Leave the library open only if it is already carrying an
	 * open screen; otherwise hand it straight back, so an AGA-only session
	 * never keeps cybergraphics.library referenced for nothing. */
	if (g_screen == NULL) cgx_close();

	return id != (ULONG)INVALID_ID;
}

/* Take the lock once, look at what we actually got, release it, and decide
 * which blit method this screen will use. Deliberately a real lock rather than
 * an inspection of the mode: the mode says what was requested, the lock says
 * what LockBitMapTagList will hand the blit every frame. */
static void probe_rtg_lock(void)
{
	APTR  handle;
	APTR  base   = NULL;
	ULONG bpr    = 0;
	ULONG depth  = 0;
	ULONG pixfmt = (ULONG)~0;
	struct TagItem tags[5];

	g_rtg_method = RTG_METHOD_WLUT;
	g_rtg_demoted = 0;

	tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
	tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
	tags[2].ti_Tag = LBMI_DEPTH;       tags[2].ti_Data = (ULONG)&depth;
	tags[3].ti_Tag = LBMI_PIXFMT;      tags[3].ti_Data = (ULONG)&pixfmt;
	tags[4].ti_Tag = TAG_DONE;         tags[4].ti_Data = 0;

	handle = LockBitMapTagList((APTR)g_screen->RastPort.BitMap, tags);
	if (handle != NULL) UnLockBitMap(handle);   /* nothing at all in between */

	if (handle == NULL) {
		amigagfx_log("RTG: LockBitMap refused - using WriteLUTPixelArray");
		return;
	}
	if (base == NULL || depth != 8UL || pixfmt != PIXFMT_LUT8 ||
	    bpr < (ULONG)g_width) {
		fprintf(stdout, "amiga: RTG: lock gave depth=%lu pixfmt=%lu bpr=%lu base=%p"
		                " - not a usable LUT8 surface, using WriteLUTPixelArray\n",
		        (unsigned long)depth, (unsigned long)pixfmt,
		        (unsigned long)bpr, base);
		fflush(stdout);
		return;
	}

	g_rtg_method = RTG_METHOD_LOCK;
	fprintf(stdout, "amiga: RTG: direct LUT8 bitmap access, bpr %lu (chunky pitch %d)"
	                " - no chunky-to-planar at all\n",
	        (unsigned long)bpr, g_width);
	fflush(stdout);
}

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
	/* Callers pass game-area coordinates; the screen wants absolute pixels,
	 * so shift past the title bar when it is visible. The warp echo comes
	 * back window-relative, i.e. already in game-area coordinates - the
	 * C++ side's echo matching needs no offset. */
	pp.iepp_Position.Y = (WORD)(y + g_yoff);

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

/* AGA and EHB: one contiguous Chip RAM block for equally spaced bitplanes,
 * handed to Intuition with SA_BitMap. depth is DEPTH_AGA (8) or DEPTH_EHB (6);
 * six additionally sets the chipset's Extra-Half-Brite bit in the mode id, which
 * is what makes registers 32..63 half-intensity copies of 0..31 and is the
 * difference between a real EHB screen and a 6-plane screen that merely uses 64
 * pens. Sets g_bpr / g_planesize / g_chip / g_bitmap / g_depth - none of which
 * exist on the RTG path, where display memory lives on the card and Chip RAM
 * must not be touched at all. Returns 0 on success. */
static int open_screen_aga(int w, int h, ULONG quiet, ULONG title, int depth)
{
	g_bpr    = ((w + 15) >> 4) << 1;
	g_planesize = (ULONG)g_bpr * h;   /* planes ALWAYS cover the full screen */

	g_chip = (UBYTE *)AllocMem(g_planesize * depth, MEMF_CHIP | MEMF_CLEAR);
	if (g_chip == NULL) return 2;

	InitBitMap(&g_bitmap, depth, w, h);
	{
		int i;
		for (i = 0; i < depth; i++)
			g_bitmap.Planes[i] = (PLANEPTR)(g_chip + (ULONG)i * g_planesize);
	}

	/* Pick the display mode from the size actually asked for, instead of always
	 * forcing hires-interlaced. 320-wide means lores, and anything at or below
	 * 256 lines fits a PAL frame without interlace - which also means no
	 * flicker, the main reason to want a lores mode in the first place. So
	 * 320x256 and 352x272 come out lores and 640x480 hires interlaced, with no
	 * special-casing anywhere. Kept general on purpose: it derives the mode from
	 * the size rather than from a table, so it stays correct whatever the
	 * resolution list happens to offer.
	 *
	 * EXTRAHALFBRITE_KEY is a LORES-only key - graphics/modeid.h defines it and
	 * EXTRAHALFBRITELACE_KEY and nothing else, because the chipset has no hires
	 * EHB. The resolution list is what keeps that promise: it offers EHB at
	 * lores widths only, so the OR below never produces an undefined mode. */
	{
		ULONG modeid = PAL_MONITOR_ID;
		modeid |= (w > 400) ? HIRES_KEY : LORES_KEY;
		if (h > 300) modeid |= 0x0004;          /* LACE bit */
		if (depth == DEPTH_EHB) modeid |= EXTRAHALFBRITE_KEY;
		g_want_modeid = modeid;

		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)depth,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     quiet,
		                          SA_ShowTitle, title,
		                          SA_Title,     (ULONG)g_screen_title,
		                          SA_DetailPen, 15UL,
		                          SA_BlockPen,  17UL,
		                          SA_Pens,      (ULONG)g_screen_pens,
		                          SA_DisplayID, modeid,
		                          TAG_END);
	}
	if (g_screen == NULL && depth == DEPTH_AGA) {
		/* Fallback: the system picks. NOTE this drops our mode entirely, so a
		 * lores request silently becomes whatever Workbench runs - usually hires.
		 *
		 * Deliberately NOT done for EHB. Without SA_DisplayID Intuition picks a
		 * mode with no Extra-Half-Brite bit, and a 6-plane screen in an ordinary
		 * mode wants 64 independently settable registers - which ECS does not
		 * have at all, and which our palette is not written for either, since
		 * entries 32..63 are deliberately the hardware halves. The honest answer
		 * to a refused EHB mode is to report failure and let amigagfx_open retry
		 * at 8 planes, where the same reduced sprites still display correctly. */
		g_used_fallback = 1;
		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)depth,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     quiet,
		                          SA_ShowTitle, title,
		                          SA_Title,     (ULONG)g_screen_title,
		                          SA_DetailPen, 15UL,
		                          SA_BlockPen,  17UL,
		                          SA_Pens,      (ULONG)g_screen_pens,
		                          TAG_END);
	}
	if (g_screen == NULL) {
		FreeMem(g_chip, g_planesize * depth); g_chip = NULL;
		if (depth == DEPTH_EHB) {
			fprintf(stdout, "amiga: EHB: OpenScreen refused mode $%08lx at %dx%d"
			                " - falling back to 8 bitplanes\n",
			        (unsigned long)g_want_modeid, w, h);
			fflush(stdout);
			return 7;
		}
		return 3;
	}

	g_depth   = depth;
	g_backend = (depth == DEPTH_EHB) ? AMIGAGFX_BACKEND_EHB : AMIGAGFX_BACKEND_AGA;

	/* Scratch band for the 6-plane c2p, which takes no chunky rowmod. Allocated
	 * with the screen so the blit never allocates; if it cannot be had, the EHB
	 * screen is not opened at all rather than blitting only full-width
	 * rectangles and leaving the rest of the display stale. */
	if (depth == DEPTH_EHB) {
		g_ehb_scratch_rows = EHB_SCRATCH_ROWS;
		g_ehb_scratch = (UBYTE *)AllocVec((ULONG)w * g_ehb_scratch_rows, MEMF_ANY);
		if (g_ehb_scratch == NULL) {
			CloseScreen(g_screen); g_screen = NULL;
			FreeMem(g_chip, g_planesize * depth); g_chip = NULL;
			g_ehb_scratch_rows = 0;
			amigagfx_log("EHB: no memory for the c2p scratch band"
			             " - falling back to 8 bitplanes");
			return 8;
		}
	}
	return 0;
}

/* RTG: an 8-bit CyberGraphX/Picasso96 screen. Deliberately NO SA_BitMap - the
 * card's display memory is allocated by CGX behind the mode id, so not one byte
 * of Chip RAM is spent here. Returns 0 on success; any non-zero means the
 * caller should open an AGA screen instead. */
static int open_screen_rtg(int w, int h, ULONG quiet, ULONG title)
{
	ULONG modeid = rtg_best_mode(w, h);

	if (modeid == (ULONG)INVALID_ID) {
		fprintf(stdout, "amiga: RTG: no 8-bit mode for %dx%d - falling back to AGA\n", w, h);
		fflush(stdout);
		return 5;
	}
	g_want_modeid = modeid;

	g_screen = OpenScreenTags(NULL,
	                          SA_Width,     (ULONG)w,
	                          SA_Height,    (ULONG)h,
	                          SA_Depth,     (ULONG)DEPTH_AGA,
	                          SA_Type,      (ULONG)CUSTOMSCREEN,
	                          SA_Quiet,     quiet,
	                          SA_ShowTitle, title,
	                          SA_Title,     (ULONG)g_screen_title,
	                          SA_DetailPen, 15UL,
	                          SA_BlockPen,  17UL,
	                          SA_Pens,      (ULONG)g_screen_pens,
	                          SA_DisplayID, modeid,
	                          TAG_END);
	if (g_screen == NULL) {
		fprintf(stdout, "amiga: RTG: OpenScreen refused mode $%08lx at %dx%d"
		                " - falling back to AGA\n", (unsigned long)modeid, w, h);
		fflush(stdout);
		return 6;
	}

	/* Nominal only: nothing on the RTG path allocates bitplanes or frees by
	 * depth - the card owns the display memory - but g_depth must not be left
	 * reading 6 from a previous EHB screen. */
	g_depth   = DEPTH_AGA;
	g_backend = AMIGAGFX_BACKEND_RTG;
	probe_rtg_lock();
	return 0;
}

int amigagfx_open(int w, int h, int show_bar, int backend)
{
	/* SA_Quiet must be OFF when the bar is wanted, or Intuition renders no
	 * screen gadgetry at all; SA_ShowTitle keeps the bar in front of our
	 * backdrop window. The bar is Intuition's own - never a drawn imitation. */
	ULONG quiet = show_bar ? FALSE : TRUE;
	ULONG title = show_bar ? TRUE  : FALSE;
	int err;

	fprintf(stdout, "amiga: amigagfx_open(%d,%d) wb_bar=%d backend=%s\n",
	        w, h, show_bar,
	        backend == AMIGAGFX_BACKEND_RTG ? "RTG" :
	        backend == AMIGAGFX_BACKEND_EHB ? "EHB" : "AGA");
	fflush(stdout);
	g_width  = w;
	g_height = h;            /* provisional; reduced below if the bar shows */
	g_yoff   = 0;
	g_used_fallback = 0;
	g_backend = AMIGAGFX_BACKEND_AGA;
	g_depth   = DEPTH_AGA;
	g_bpr = 0;
	g_planesize = 0;
	g_epoch  = raw_ticks();

	/* Ask for what was requested; fall back to a plain 8-bitplane AGA screen
	 * whenever that did not work out. A machine with no graphics card therefore
	 * behaves exactly as it did before RTG support existed, one without the
	 * chipset mode behaves as it did before EHB did, and one log line always
	 * says why. */
	switch (backend) {
		case AMIGAGFX_BACKEND_RTG:
			err = open_screen_rtg(w, h, quiet, title);
			break;
		case AMIGAGFX_BACKEND_EHB:
			err = open_screen_aga(w, h, quiet, title, DEPTH_EHB);
			break;
		default:
			err = 5;
			break;
	}
	if (err != 0) {
		if (backend == AMIGAGFX_BACKEND_RTG) cgx_close();
		err = open_screen_aga(w, h, quiet, title, DEPTH_AGA);
		if (err != 0) { amigagfx_close(); return err; }
	}

	/* The bar height is whatever Intuition says it is for the mode and font it
	 * actually opened with - it is NEVER hard-coded. The bar itself occupies
	 * BarHeight+1 lines (BarHeight excludes the separator line), so the game
	 * area starts right below that and everything downstream (chunky buffer,
	 * dirty rects, blit, mouse warp) works in the reduced height. */
	if (show_bar) {
		int bar = (int)g_screen->BarHeight + 1;
		if (bar > 0 && bar < h / 2) {
			g_yoff   = bar;
			g_height = h - bar;
			fprintf(stdout, "amiga: wb bar visible, BarHeight %d -> game area %dx%d at y=%d\n",
			        (int)g_screen->BarHeight, g_width, g_height, g_yoff);
		} else {
			fprintf(stdout, "amiga: wb bar height %d implausible for %d lines - bar ignored\n",
			        bar, h);
		}
		fflush(stdout);
	}

	{
		/* Report the mode Intuition ACTUALLY granted, not the one we asked for.
		 * It substitutes silently when a mode is unavailable, and a lores-sized
		 * screen running in a hires mode looks identical in a log that only
		 * prints the pixel dimensions. */
		ULONG got = GetVPModeID(&g_screen->ViewPort);
		if (g_backend == AMIGAGFX_BACKEND_RTG) {
			fprintf(stdout, "amiga: RTG screen open %dx%d depth 8 (no Chip RAM, no c2p)\n", w, h);
			fprintf(stdout, "amiga: modeid wanted $%08lx got $%08lx  blit=%s\n",
			        (unsigned long)g_want_modeid, (unsigned long)got,
			        g_rtg_method == RTG_METHOD_LOCK ? "LockBitMap+memcpy"
			                                        : "WriteLUTPixelArray");
		} else {
			fprintf(stdout, "amiga: %s screen open %dx%d depth %d, bpr %d,"
			                " chip %lu KB\n",
			        g_backend == AMIGAGFX_BACKEND_EHB ? "EHB" : "AGA",
			        w, h, g_depth, g_bpr,
			        (unsigned long)((g_planesize * g_depth) >> 10));
			fprintf(stdout, "amiga: modeid wanted $%08lx got $%08lx  %s%s%s  [%s]\n",
			        (unsigned long)g_want_modeid, (unsigned long)got,
			        (got & HIRES_KEY) ? "HIRES " : "LORES ",
			        (got & 0x0004) ? "INTERLACED" : "non-interlaced",
			        (got & EXTRAHALFBRITE_KEY) ? " EHB" : "",
			        g_used_fallback ? "SYSTEM FALLBACK - our mode was refused"
			                        : "our mode accepted");
			/* The EHB bit is the whole mode. If Intuition granted the screen but
			 * dropped it, registers 32..63 are NOT hardware halves and half the
			 * palette is wrong - so say so loudly rather than let it look like a
			 * palette bug later. */
			if (g_backend == AMIGAGFX_BACKEND_EHB && !(got & EXTRAHALFBRITE_KEY)) {
				amigagfx_log("EHB: WARNING - granted mode has no EXTRAHALFBRITE bit;"
				             " colours 32..63 will not be hardware half-brights");
			}
		}
		fflush(stdout);
	}

	/* Chunky buffer covers the GAME AREA only, so any write past its bounds is
	 * a bug, not silently absorbed border space. Allocated after the screen
	 * opens because only then is the bar height - and thus g_height - known. */
	g_chunky = (UBYTE *)AllocVec((ULONG)g_width * g_height, MEMF_ANY | MEMF_CLEAR);
	if (g_chunky == NULL) { amigagfx_close(); return 1; }

	/* The input window covers only the game area, leaving the title bar to
	 * Intuition: clicks on the depth gadget and screen drags stay system-
	 * handled, and IDCMP mouse coordinates (window-relative) automatically
	 * become game-area coordinates. */
	g_window = OpenWindowTags(NULL,
	                          WA_CustomScreen, (ULONG)g_screen,
	                          WA_Left, 0UL, WA_Top, (ULONG)g_yoff,
	                          WA_Width, (ULONG)w, WA_Height, (ULONG)g_height,
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
	/* Chip RAM only ever exists on the AGA path; on RTG g_chip stays NULL and
	 * this is a no-op, which is exactly the point - the card owns the display
	 * memory and none of the machine's scarce Chip RAM is spent on it. */
	if (g_chip != NULL)   { FreeMem(g_chip, g_planesize * g_depth); g_chip = NULL; }
	if (g_chunky != NULL) { FreeVec(g_chunky); g_chunky = NULL; }
	if (g_ehb_scratch != NULL) { FreeVec(g_ehb_scratch); g_ehb_scratch = NULL; }
	g_ehb_scratch_rows = 0;
	g_depth = DEPTH_AGA;
	/* The screen is gone, so nothing can be locked any more; hand the library
	 * back. A resolution change closes and reopens it, which costs one
	 * OpenLibrary on an already-resident library - not worth keeping state for. */
	cgx_close();
	g_backend = AMIGAGFX_BACKEND_AGA;
	g_yoff = 0;
}

unsigned char *amigagfx_chunky(void) { return g_chunky; }
int amigagfx_pitch(void) { return g_width; }
int amigagfx_game_height(void) { return g_height; }

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
		/* Mirror into the CTABFMT_XRGB8 table WriteLUTPixelArray needs. Kept
		 * up to date unconditionally rather than only on RTG: it costs three
		 * shifts per changed colour and means the WLUT fallback is always
		 * ready, including when a mid-session lock failure demotes to it. */
		g_ctable[first + i] = ((ULONG)rgb[i*3 + 0] << 16) |
		                      ((ULONG)rgb[i*3 + 1] <<  8) |
		                       (ULONG)rgb[i*3 + 2];
	}
	table[1 + count * 3] = 0UL;
	LoadRGB32(&g_screen->ViewPort, table);
}

void amigagfx_set_ehb_palette(const unsigned char *rgb64)
{
	if (rgb64 == NULL) { g_ehb_pal_valid = 0; return; }
	memcpy(g_ehb_pal, rgb64, sizeof(g_ehb_pal));
	g_ehb_pal_valid = 1;
}

/* Nearest EHB pen for one RGB triple, plain squared distance over the 64
 * entries. Used only by the splash and only once per colour in its palette
 * (at most 256), so 16k distance computations for the whole image - nothing
 * worth a smarter search on a 68030. */
static UBYTE ehb_nearest(int r, int g, int b)
{
	int best = 0, bestd = 0x7fffffff, i;

	for (i = 0; i < 64; i++) {
		int dr = r - (int)g_ehb_pal[i * 3 + 0];
		int dg = g - (int)g_ehb_pal[i * 3 + 1];
		int db = b - (int)g_ehb_pal[i * 3 + 2];
		int d  = dr * dr + dg * dg + db * db;
		if (d < bestd) { bestd = d; best = i; }
	}
	return (UBYTE)best;
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
	UBYTE ehb_map[256];          /* file palette index -> EHB pen, EHB only */
	const UBYTE *fade_pal;       /* palette the fade actually animates */
	int fade_ncol;
	int is_ehb;
	int w, h, ncol, scale, dx, dy, x, y;
	char msg[128];

	if (g_screen == NULL || g_chunky == NULL) return;

	/* The splash must render in whatever mode the game opened, never the other
	 * way round: the entire audience for the EHB mode is machines that cannot
	 * show an 8-bitplane screen, so a splash that insisted on one would fail
	 * before the game started. On EHB the IMAGE is reduced to the screen's 64
	 * pens; the screen is not changed to suit the image. */
	is_ehb = (g_backend == AMIGAGFX_BACKEND_EHB);
	if (is_ehb && !g_ehb_pal_valid) {
		amigagfx_log("splash: EHB screen but no EHB palette was handed over - skipped");
		return;
	}

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

	/* Which palette the screen will actually be showing, and therefore which one
	 * the fade animates.
	 *
	 * On AGA and RTG that is the file's own palette, loaded as-is - unchanged
	 * behaviour. On EHB it cannot be: there are 64 pens, and 32 of them are
	 * hardware halves that cannot be set at all, so an arbitrary 256-colour
	 * palette is simply not loadable. The image is therefore reduced to the EHB
	 * pens once, here, and the fade then scales the EHB palette. Because entries
	 * 32..63 of that palette are exactly the half-intensity images of 0..31,
	 * scaling all 64 by one factor keeps the table consistent with what the
	 * chipset derives - so the fade needs no special case of its own. */
	if (is_ehb) {
		int i;
		for (i = 0; i < ncol; i++) {
			ehb_map[i] = ehb_nearest((int)pal[i * 3 + 0],
			                         (int)pal[i * 3 + 1],
			                         (int)pal[i * 3 + 2]);
		}
		/* Index 0 is the border and must stay black in the reduced image too,
		 * or the surround fades up to whatever happened to be nearest. */
		ehb_map[0] = 0;
		fade_pal  = g_ehb_pal;
		fade_ncol = 64;
	} else {
		fade_pal  = pal;
		fade_ncol = ncol;
	}

	/* Black the palette out BEFORE the image reaches the screen, so the fade
	 * starts from darkness instead of flashing the game palette. */
	splash_palette_step(fade_pal, fade_ncol, 0);

	/* Compose once: black border (index 0), image centred. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	dx = (g_width  - w * scale) / 2;
	dy = (g_height - h * scale) / 2;
	for (y = 0; y < h; y++) {
		const UBYTE *src = pix + (ULONG)y * w;
		UBYTE *dst = g_chunky + (ULONG)(dy + y * scale) * g_width + dx;
		if (scale == 1) {
			if (is_ehb) {
				for (x = 0; x < w; x++) dst[x] = ehb_map[src[x]];
			} else {
				memcpy(dst, src, (size_t)w);
			}
		} else {
			UBYTE *d = dst;
			if (is_ehb) {
				for (x = 0; x < w; x++) { UBYTE c = ehb_map[src[x]]; *d++ = c; *d++ = c; }
			} else {
				for (x = 0; x < w; x++) { UBYTE c = src[x]; *d++ = c; *d++ = c; }
			}
			memcpy(dst + g_width, dst, (size_t)w * 2);   /* double the row */
		}
	}
	FreeVec(pix);
	pix = NULL;

	/* The ONE chunky-to-planar conversion of the splash. */
	amigagfx_blit(0, 0, g_width, g_height);

	snprintf(msg, sizeof(msg), "splash: %dx%d ncol %d at %d,%d scale %dx%s",
	         w, h, ncol, dx, dy, scale, is_ehb ? " (reduced to 64 EHB pens)" : "");
	amigagfx_log(msg);

	/* Fade in, hold, fade out - palette-only from here on. */
	splash_fade(fade_pal, fade_ncol, 0, 256, SPLASH_FADE_MS);
	Delay(SPLASH_HOLD_TICKS);
	splash_fade(fade_pal, fade_ncol, 256, 0, SPLASH_FADE_MS);

	/* Leave the screen genuinely black: clear the chunky buffer and convert
	 * once more, so restoring the game palette cannot flash the image back. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	amigagfx_blit(0, 0, g_width, g_height);
}

/* RTG blit, direct route: lock the screen's bitmap, memcpy the rectangle row by
 * row, unlock. On an 8-bit LUT8 surface our chunky bytes ARE the pen numbers,
 * so there is no conversion of any kind - this is the whole reason RTG is worth
 * having. Returns 0 if the lock could not be used, which permanently demotes
 * this screen to the WriteLUTPixelArray path.
 *
 * Read the LOCKING DISCIPLINE comment at the top of the RTG state block before
 * touching this. In short: every bound is computed BEFORE the lock, nothing but
 * memcpy happens inside it, and the single UnLockBitMap is reached on every
 * path including the "wrong format" one. */
static int rtg_blit_locked(int x, int y, int w, int h)
{
	APTR  handle;
	APTR  base   = NULL;
	ULONG bpr    = 0;
	ULONG depth  = 0;
	ULONG pixfmt = (ULONG)~0;
	struct TagItem tags[5];
	int   usable;

	tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
	tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
	tags[2].ti_Tag = LBMI_DEPTH;       tags[2].ti_Data = (ULONG)&depth;
	tags[3].ti_Tag = LBMI_PIXFMT;      tags[3].ti_Data = (ULONG)&pixfmt;
	tags[4].ti_Tag = TAG_DONE;         tags[4].ti_Data = 0;

	handle = LockBitMapTagList((APTR)g_screen->RastPort.BitMap, tags);
	if (handle == NULL) return 0;

	/* ---- LOCK HELD: memcpy only, no OS calls, no early return ---- */
	usable = (base != NULL && depth == 8UL && pixfmt == PIXFMT_LUT8 &&
	          bpr >= (ULONG)(x + w));
	if (usable) {
		const UBYTE *src = g_chunky + (ULONG)y * g_width + x;
		/* The destination row is the SCREEN row: the chunky buffer covers the
		 * game area only, so the title bar offset is added here and nowhere
		 * else. bpr is the card's pitch and is NOT g_width - it is whatever
		 * this lock reported, re-read every time, never cached. */
		UBYTE *dst = (UBYTE *)base + (ULONG)(y + g_yoff) * bpr + x;
		int row;
		for (row = 0; row < h; row++) {
			memcpy(dst, src, (size_t)w);
			src += g_width;
			dst += bpr;
		}
	}
	UnLockBitMap(handle);
	/* ---- LOCK RELEASED ---- */

	return usable;
}

/* RTG blit, library route. Correct on any CGX screen but goes through the
 * library per rectangle and is specified in colours rather than pen numbers, so
 * it may remap every pixel where the locked path copies it. Only ever reached
 * when the lock is unavailable. */
static void rtg_blit_wlut(int x, int y, int w, int h)
{
	WriteLUTPixelArray((APTR)g_chunky,
	                   (UWORD)x, (UWORD)y, (UWORD)g_width,
	                   &g_screen->RastPort, (APTR)g_ctable,
	                   (UWORD)x, (UWORD)(y + g_yoff),
	                   (UWORD)w, (UWORD)h,
	                   (UBYTE)CTABFMT_XRGB8);
}

/* EHB blit: six bitplanes through Kalms' stock c2p1x1_6_c5_bm_040.
 *
 * That routine takes no chunky ROWMOD - unlike the 8-plane c2p_rect, which is
 * the only Kalms routine that does - so it insists its input be contiguous and
 * exactly chunkyx wide. Two cases follow, and the first one is why this is
 * cheap in practice:
 *
 *   full-width rectangle (x == 0, w == g_width): the rows of the chunky buffer
 *     ARE contiguous, so the buffer is handed over directly and nothing is
 *     copied at all. Most large dirty rectangles - and every full-screen
 *     redraw, which is the expensive case - go this way.
 *
 *   narrower rectangle: the rows are copied into a scratch band first, in
 *     chunks of at most EHB_SCRATCH_ROWS rows, and the routine is called once
 *     per band. One extra memcpy of exactly the pixels being converted.
 *
 * Copying rather than hand-writing a 6-plane rect routine is a deliberate
 * choice: this is released, validated assembly, and a memcpy is far cheaper
 * than the class of bug a bespoke c2p would introduce. */
static void ehb_blit(int x, int y, int w, int h)
{
	struct C2P6Args a;

	a.chunkyx = (UWORD)w;
	a.offsx   = (UWORD)x;
	a.bitmap  = (APTR)&g_bitmap;

	if (x == 0 && w == g_width) {
		a.chunkyy = (UWORD)h;
		a.offsy   = (UWORD)(y + g_yoff);
		a.chunky  = (APTR)(g_chunky + (ULONG)y * g_width);
		c2p6_bm_asm(&a);
		return;
	}

	while (h > 0) {
		int band = (h < g_ehb_scratch_rows) ? h : g_ehb_scratch_rows;
		const UBYTE *src = g_chunky + (ULONG)y * g_width + x;
		UBYTE *dst = g_ehb_scratch;
		int row;

		for (row = 0; row < band; row++) {
			memcpy(dst, src, (size_t)w);
			src += g_width;
			dst += w;
		}

		a.chunkyy = (UWORD)band;
		a.offsy   = (UWORD)(y + g_yoff);
		a.chunky  = (APTR)g_ehb_scratch;
		c2p6_bm_asm(&a);

		y += band;
		h -= band;
	}
}

void amigagfx_blit(int x, int y, int w, int h)
{
	struct C2PArgs args;
	int x2, y2;

	if (g_screen == NULL) return;

	x2 = x + w;
	y2 = y + h;

	/* The 32-pixel column granularity below is a property of the two c2p
	 * routines and of nothing else, so RTG must not pay for it: an RTG rectangle
	 * is used as given and only clipped. Snapping it outwards there would
	 * convert pixels that never changed, for no reason at all.
	 *
	 * Both planar routines need it, and both are satisfied by the same snap:
	 * c2p_rect wants x and width on a 32-pixel grid, and the 6-plane routine
	 * wants a width that is a multiple of 32 and an x offset that is a multiple
	 * of 8 - which a multiple of 32 already is. */
	if (g_backend != AMIGAGFX_BACKEND_RTG) {
		/* Kalms' c2p works on 32-pixel columns: grow the rect outwards to that
		 * grid rather than refusing it, then clip to the screen. */
		x  &= ~31;
		x2  = (x2 + 31) & ~31;
	}

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > g_width)  x2 = g_width;
	if (y2 > g_height) y2 = g_height;
	if (x2 <= x || y2 <= y) return;

	if (g_backend == AMIGAGFX_BACKEND_RTG) {
		if (g_rtg_method == RTG_METHOD_LOCK &&
		    !rtg_blit_locked(x, y, x2 - x, y2 - y)) {
			/* Demote once and for this screen only. Logged outside the lock. */
			g_rtg_method = RTG_METHOD_WLUT;
			if (!g_rtg_demoted) {
				g_rtg_demoted = 1;
				amigagfx_log("RTG: LockBitMap became unusable - "
				             "switching to WriteLUTPixelArray for this screen");
			}
		}
		if (g_rtg_method == RTG_METHOD_WLUT) rtg_blit_wlut(x, y, x2 - x, y2 - y);

		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "amiga: rtg blit #%lu  %dx%d at %d,%d\n",
			        g_blits, x2 - x, y2 - y, x, y);
			fflush(stdout);
		}
		return;
	}

	if (g_backend == AMIGAGFX_BACKEND_EHB) {
		ehb_blit(x, y, x2 - x, y2 - y);

		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "amiga: ehb blit #%lu  %dx%d at %d,%d  %s\n",
			        g_blits, x2 - x, y2 - y, x, y,
			        (x == 0 && x2 == g_width) ? "direct" : "via scratch band");
			fflush(stdout);
		}
		return;
	}

	args.x = (UWORD)x;
	args.y = (UWORD)y;
	args.w = (UWORD)(x2 - x);
	args.h = (UWORD)(y2 - y);
	args.cmod = (UWORD)g_width;
	args.bmod = (UWORD)g_bpr;
	args.bplsize = g_planesize;
	args.chunky = g_chunky;
	/* Skip the title bar lines when the bar is visible: plane spacing
	 * (bplsize) stays the full-screen plane size, only the row origin moves.
	 * g_bpr is a multiple of 4 for every offered width, so the 32-pixel column
	 * alignment of the c2p is unaffected. */
	args.bpl = g_chip + (ULONG)g_yoff * g_bpr;
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
