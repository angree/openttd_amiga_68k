/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file amiga_v.cpp Native AmigaOS (AGA) video driver.
 *
 * OpenTTD's 8bpp blitter already renders into a chunky palette-index buffer,
 * which is precisely the input a chunky-to-planar converter wants, so this
 * driver simply hands the blitter a Fast RAM buffer and pushes dirty rectangles
 * through Kalms' 68040 c2p onto an AGA screen.
 *
 * All Amiga API calls live behind amiga_gfx.h - including Amiga headers here
 * would drag in macros and types that collide with OpenTTD's own names.
 */

#include "../stdafx.h"
#include <stdio.h>      /* snprintf for the diagnostic log lines */
#include <stdlib.h>     /* atoi for the frameskip driver parameter */
#include "../openttd.h"
#include "../gfx_func.h"
#include "../variables.h"
#include "../rev.h"
#include "../blitter/factory.hpp"
#include "../network/network.h"
#include "../functions.h"
#include "../genworld.h"
#include "../core/random_func.hpp"
#include "../fontcache.h"
#include "../settings_func.h" /* SaveToConfig - see ChangeResolution */
#include "../settings_type.h" /* _settings_client.amiga.wb_bar */
#include "amiga_v.h"
#include "amiga_gfx.h"

/**
 * The modes offered in Game Options -> Screen resolution.
 *
 * AGA widths are all multiples of 32 because Kalms' c2p converts 32-pixel
 * columns; 368, the other common PAL overscan width, is not and would be
 * rejected. That constraint is a c2p property and is NOT applied to RTG.
 *
 *   320x256  plain PAL lores, no interlace - no flicker
 *   352x272  PAL lores with overscan: same mode, bigger window
 *   640x480  PAL hires interlaced - the readable, default choice
 *
 * 640x512 is gone: it is 640x480 with 32 more lines, at a real cost (it was the
 * most expensive mode to convert), and 640x480 covers the same ground.
 *
 * A 640x256 entry was tried and REMOVED. As a real resolution it is 2.5:1, and
 * the game laid out in it is badly stretched - the aspect ratio must not be
 * sacrificed for conversion speed. The idea worth keeping from it is a
 * different one: render at a normal ratio and put alternate lines on a hires
 * NON-interlaced screen, where a pixel is twice as tall, so the picture keeps
 * its proportions. That is a blit-side trick, not a resolution, and it is
 * deliberately not implemented here.
 */
static const Dimension _amiga_aga_resolutions[] = {
	{ 320, 256 },
	{ 352, 272 },
	{ 640, 480 },
};

/**
 * RTG (CyberGraphX / Picasso96) candidates, offered BELOW the AGA modes and
 * only when the machine actually has an 8-bit RTG mode of that size - so an
 * AGA-only Amiga sees exactly the list it saw before RTG support existed.
 *
 * 8bpp only, deliberately: OpenTTD's blitter produces 8-bit chunky, which on an
 * 8-bit RTG screen is already the display format. Any deeper mode would put a
 * per-pixel conversion back in, which is the very thing RTG is here to remove.
 *
 * Nothing below 400x300: the small sizes exist for AGA, where they buy a real
 * reduction in chunky-to-planar work. On RTG there is no conversion to reduce,
 * so a tiny mode buys nothing and only costs visible map.
 */
static const Dimension _amiga_rtg_resolutions[] = {
	{  400, 300 },
	{  512, 384 },
	{  640, 480 },
	{  800, 600 },
	{ 1024, 768 },
};

/** SPECSTR_RESOLUTION_END - SPECSTR_RESOLUTION_START + 1: the hard ceiling on
 * how many entries the resolution dropdown can address. */
#define AMIGA_MAX_RESOLUTIONS 32

/**
 * Which backend each entry of _resolutions[] belongs to, kept strictly parallel
 * to it.
 *
 * This exists because _resolutions[] carries nothing but a width and a height,
 * while ChangeResInGame() and GetCurRes() identify a mode BY those two numbers -
 * and 640x480 is offered on both backends, so the pair is genuinely ambiguous on
 * an RTG machine. The dropdown index is not ambiguous, so the index is what is
 * tracked: settings_gui.cpp hands it to AmigaSetResolutionIndex() before calling
 * ChangeResInGame(), and asks AmigaGetResolutionIndex() for the entry to show as
 * current instead of searching by size.
 */
static bool _amiga_res_rtg[AMIGA_MAX_RESOLUTIONS];
static int  _amiga_cur_res;       ///< index of the mode currently on screen
static int  _amiga_pending_res;   ///< index the player just picked, -1 if none


/**
 * Amiga raw key code -> OpenTTD key code.
 *
 * Keyboard events were reaching the driver all along - they were simply never
 * forwarded, which is why only ESC appeared to work (it was hard-wired to quit).
 * Raw codes are positional and layout-independent, so this table is written
 * against the physical Amiga keyboard.
 *
 * Printable keys carry their ASCII character as well; OpenTTD wants
 * character | (keycode << 16) and uses the character for text entry.
 */
struct AmigaKey { uint8 raw; uint16 wkc; char ascii; char shifted; };

static const AmigaKey _amiga_keys[] = {
	{ 0x45, WKC_ESC,       0,   0   },
	{ 0x41, WKC_BACKSPACE, 0,   0   },
	{ 0x46, WKC_DELETE,    0,   0   },
	{ 0x44, WKC_RETURN,    0,   0   },
	{ 0x43, WKC_RETURN,    0,   0   },   /* keypad Enter */
	{ 0x42, WKC_TAB,       0,   0   },
	{ 0x40, WKC_SPACE,    ' ', ' '  },
	{ 0x4C, WKC_UP,        0,   0   },
	{ 0x4D, WKC_DOWN,      0,   0   },
	{ 0x4E, WKC_RIGHT,     0,   0   },
	{ 0x4F, WKC_LEFT,      0,   0   },
	{ 0x50, WKC_F1,        0,   0   }, { 0x51, WKC_F2,  0, 0 },
	{ 0x52, WKC_F3,        0,   0   }, { 0x53, WKC_F4,  0, 0 },
	{ 0x54, WKC_F5,        0,   0   }, { 0x55, WKC_F6,  0, 0 },
	{ 0x56, WKC_F7,        0,   0   }, { 0x57, WKC_F8,  0, 0 },
	{ 0x58, WKC_F9,        0,   0   }, { 0x59, WKC_F10, 0, 0 },

	{ 0x01, '1', '1', '!' }, { 0x02, '2', '2', '@' }, { 0x03, '3', '3', '#' },
	{ 0x04, '4', '4', '$' }, { 0x05, '5', '5', '%' }, { 0x06, '6', '6', '^' },
	{ 0x07, '7', '7', '&' }, { 0x08, '8', '8', '*' }, { 0x09, '9', '9', '(' },
	{ 0x0A, '0', '0', ')' }, { 0x0B, '-', '-', '_' }, { 0x0C, '=', '=', '+' },

	{ 0x10, 'Q', 'q', 'Q' }, { 0x11, 'W', 'w', 'W' }, { 0x12, 'E', 'e', 'E' },
	{ 0x13, 'R', 'r', 'R' }, { 0x14, 'T', 't', 'T' }, { 0x15, 'Y', 'y', 'Y' },
	{ 0x16, 'U', 'u', 'U' }, { 0x17, 'I', 'i', 'I' }, { 0x18, 'O', 'o', 'O' },
	{ 0x19, 'P', 'p', 'P' },

	{ 0x20, 'A', 'a', 'A' }, { 0x21, 'S', 's', 'S' }, { 0x22, 'D', 'd', 'D' },
	{ 0x23, 'F', 'f', 'F' }, { 0x24, 'G', 'g', 'G' }, { 0x25, 'H', 'h', 'H' },
	{ 0x26, 'J', 'j', 'J' }, { 0x27, 'K', 'k', 'K' }, { 0x28, 'L', 'l', 'L' },

	{ 0x31, 'Z', 'z', 'Z' }, { 0x32, 'X', 'x', 'X' }, { 0x33, 'C', 'c', 'C' },
	{ 0x34, 'V', 'v', 'V' }, { 0x35, 'B', 'b', 'B' }, { 0x36, 'N', 'n', 'N' },
	{ 0x37, 'M', 'm', 'M' },
	{ 0x38, ',', ',', '<' }, { 0x39, '.', '.', '>' }, { 0x3A, '/', '/', '?' },
};

/** Qualifier state, tracked from the raw codes for shift and control. */
static bool _amiga_shift, _amiga_ctrl;

static void HandleAmigaKey(int raw)
{
	uint16 wkc = 0;
	char   ch  = 0;

	for (uint i = 0; i < lengthof(_amiga_keys); i++) {
		if (_amiga_keys[i].raw != raw) continue;
		wkc = _amiga_keys[i].wkc;
		ch  = _amiga_shift ? _amiga_keys[i].shifted : _amiga_keys[i].ascii;
		break;
	}
	if (wkc == 0) return;

	if (_amiga_shift) wkc |= WKC_SHIFT;
	if (_amiga_ctrl)  wkc |= WKC_CTRL;

	HandleKeypress((uint32)(uint8)ch | ((uint32)wkc << 16));
}

/**
 * Adaptive frame skipping.
 *
 * The simulation rate is fixed and must stay that way - OpenTTD relies on it
 * for determinism and savegame compatibility, so slowing the game clock to
 * cope with a slow machine would change how the game actually plays.
 *
 * The honest lever is to draw less often. This is precisely what fast-forward
 * demonstrates: it does not make the machine faster, it lets the game loop run
 * without waiting, and the simulation catches up. Here we get the same effect
 * automatically by dropping frames only while we are behind schedule.
 *
 * "-v amiga:frameskip=N" pins it to a fixed value; the default adapts.
 */
#define MAX_FRAMESKIP 8

static int  _frameskip;        ///< frames to drop between drawn ones
static int  _skip_left;        ///< countdown to the next drawn frame
static bool _frameskip_auto;   ///< OFF by default: dropping frames looked far worse
                               ///< in practice than letting the player hit fast-forward.
static int  _fs_report;

/** Lores modes get the small interface font, hires the normal one. */
static inline bool WantSmallFont(uint w) { return w < 400; }

static FVideoDriver_Amiga iFVideoDriver_Amiga;

/**
 * Dirty rectangle list.
 *
 * This started life as a single bounding box, which was a bad simplification:
 * OpenTTD's DrawDirtyBlocks() already coalesces damage and calls MakeDirty()
 * once per resulting rectangle, so merging them all into one box re-converts
 * everything in between. A frame that really changed three small windows was
 * costing a 640x209 blit - over ten times the necessary work, and it was
 * clearly visible as sluggishness on a real-speed 68040.
 *
 * Now each rectangle is kept and converted separately. Overlapping ones are
 * merged (cheap, and avoids converting the same pixels twice); on overflow we
 * fall back to a single bounding box rather than dropping updates.
 */
#define MAX_DIRTY_RECTS 48

/** Force the old single-bounding-box behaviour with "-v amiga:bbox".
 * Having both paths in one binary means this optimisation can be switched on
 * and off between runs, so it can be evaluated on its own instead of being
 * confounded with whatever else changed in the same build. */
static bool _force_bbox;

/** "-v amiga:verbose" re-enables the per-frame diagnostics that are too chatty
 * for normal play on a 68040: the first-iterations trace in MainLoop, the
 * right-drag scroll deltas, the periodic blit-statistics summary and the
 * periodic "blit #N" heartbeat in amiga_gfx.c. Startup lines, errors and the
 * capped pointer-warp diagnostics stay on unconditionally. */
static bool _verbose;

/** Diagnostic: "-v amiga:rmbaslmb" makes the RIGHT button behave exactly like
 * the LEFT one. If right-clicking then acts like a left click, the IDCMP event
 * is arriving fine and the fault is in the scroll handling; if nothing happens
 * at all, the button never reaches us and the problem is below our code. */
static bool _rmb_as_lmb;

/** Last physical pointer position, needed because Intuition cannot warp the
 * mouse: during fix_at scrolling _cursor.pos stays anchored, so deltas have
 * to be measured against this instead. */
static int _phys_x, _phys_y;
static int _dbg_scroll;

/** Pointer warp bookkeeping for right-drag scrolling.
 *
 * While _cursor.fix_at is set only deltas matter, but the SYSTEM pointer still
 * moves and would eventually hit the screen border, where the deltas drop to
 * zero and the scroll dead-ends. So after every drained batch of movement we
 * push it back to the screen centre through input.device
 * (amigagfx_warp_pointer, IECLASS_NEWPOINTERPOS + IESUBCLASS_PIXEL): the
 * pointer then hovers around the centre for the whole drag and can never even
 * approach an edge. This mirrors SDL's warp-based relative mouse mode.
 *
 * The warp itself comes back to us as a perfectly ordinary IDCMP mouse-move,
 * and reporting THAT as movement would jump the map by half a screen. Hence
 * the pending flag: remember the exact target and swallow the first move that
 * lands on it, resyncing _phys_* instead of producing a delta. Events already
 * queued before the warp carry pre-warp coordinates and are processed first
 * (the input stream is ordered), so their deltas against _phys_* stay valid.
 * Should the echo ever get lost (warp silently ignored), a stuck pending flag
 * would block all further warps - so after WARP_ECHO_LIMIT non-matching moves
 * it is cleared, logged, and the next poll simply warps again. */
static bool _warp_pending;
static int  _warp_x, _warp_y;
static int  _warp_misses;
static int  _dbg_warp_trig, _dbg_warp_echo;
#define WARP_ECHO_LIMIT 20

struct DirtyRect { int left, top, right, bottom; };
static DirtyRect _dirty[MAX_DIRTY_RECTS];
static int  _num_dirty;
static bool _dirty_overflow;
static int  _bbox_left, _bbox_top, _bbox_right, _bbox_bottom;

static inline void ResetDirty()
{
	_num_dirty = 0;
	_dirty_overflow = false;
	_bbox_left = _bbox_top = 0x7fffffff;
	_bbox_right = _bbox_bottom = 0;
}

/** Do the two rectangles touch or overlap? Merging those is always a win. */
static inline bool RectsIntersect(const DirtyRect *a, int left, int top, int right, int bottom)
{
	return !(a->left > right || a->right < left || a->top > bottom || a->bottom < top);
}

static void UpdatePalette(uint start, uint count)
{
	uint8 rgb[256 * 3];
	uint i;

	if (count > 256) count = 256;
	for (i = 0; i < count; i++) {
		rgb[i * 3 + 0] = _cur_palette[start + i].r;
		rgb[i * 3 + 1] = _cur_palette[start + i].g;
		rgb[i * 3 + 2] = _cur_palette[start + i].b;
	}
	amigagfx_set_palette(rgb, (int)start, (int)count);
}

static void CheckPaletteAnim()
{
	if (_pal_count_dirty == 0) return;

	Blitter *blitter = BlitterFactoryBase::GetCurrentBlitter();
	switch (blitter->UsePaletteAnimation()) {
		case Blitter::PALETTE_ANIMATION_VIDEO_BACKEND:
			UpdatePalette(_pal_first_dirty, _pal_count_dirty);
			break;

		case Blitter::PALETTE_ANIMATION_BLITTER:
			blitter->PaletteAnimate(_pal_first_dirty, _pal_count_dirty);
			break;

		case Blitter::PALETTE_ANIMATION_NONE:
			break;

		default:
			NOT_REACHED();
	}
	_pal_count_dirty = 0;
}

/* The per-200-frames blit statistics ("N kpx converted vs M kpx for one bbox")
 * are gone for release: they had already proven their point - the rectangle
 * list beats a single bounding box - and both the area sums every frame and
 * the log I/O were measurable overhead on a 68040. To re-measure, run with
 * "-v amiga:bbox" and compare feel/timings; the A/B switch is still here. */

static void DrawSurfaceToScreen()
{
	if (_dirty_overflow || _force_bbox) {
		if (_bbox_right > _bbox_left && _bbox_bottom > _bbox_top) {
			amigagfx_blit(_bbox_left, _bbox_top,
			              _bbox_right - _bbox_left, _bbox_bottom - _bbox_top);
		}
	} else {
		for (int i = 0; i < _num_dirty; i++) {
			amigagfx_blit(_dirty[i].left, _dirty[i].top,
			              _dirty[i].right - _dirty[i].left,
			              _dirty[i].bottom - _dirty[i].top);
		}
	}
	ResetDirty();
}

/** SCREEN size of the currently open screen (the size in the resolution list),
 * as opposed to _screen.width/height, which is the GAME AREA - smaller by the
 * system title bar when amiga.wb_bar is on. Kept so both the wb_bar toggle and
 * ChangeResolution can reopen/persist by screen size without shrinking it by
 * one bar height per reopen. Zero until the driver has started. */
static uint _amiga_scr_w, _amiga_scr_h;

static bool CreateMainSurface(uint w, uint h, bool want_rtg)
{
	/* The c2p converts 32 pixels at a time, so an AGA width must be a multiple
	 * of 32. Round up rather than fail; 640 and 800 already qualify. RTG has no
	 * such constraint - nothing there works in 32-pixel columns - so its widths
	 * are passed through exactly as offered. */
	if (!want_rtg) w = (w + 31) & ~31;

	amigagfx_close();
	/* amiga.wb_bar: keep the real Intuition screen title bar (depth gadget,
	 * flip to Workbench) visible. Read at open time; the toggle proc reopens
	 * the screen through this very function, like a resolution change. */
	int err = amigagfx_open((int)w, (int)h, _settings_client.amiga.wb_bar ? 1 : 0,
	                        want_rtg ? AMIGAGFX_BACKEND_RTG : AMIGAGFX_BACKEND_AGA);
	if (err != 0) {
		DEBUG(driver, 0, "amiga: could not open a %dx%d 256-colour screen (error %d)", w, h, err);
		return false;
	}

	/* amigagfx_open falls back to AGA silently when RTG is unavailable, so ask
	 * what actually opened rather than assuming we got what we asked for. */
	bool got_rtg = (amigagfx_backend() == AMIGAGFX_BACKEND_RTG);
	if (want_rtg && !got_rtg) {
		amigagfx_log("RTG requested but AGA opened - see the reason logged above");
	}
	_settings_client.amiga.rtg = got_rtg;

	_amiga_scr_w = w;
	_amiga_scr_h = h;

	/* The game area is what the driver reports: with the bar visible it is
	 * shorter than the screen, and every downstream consumer - the chunky
	 * buffer, dirty rects, c2p, window layout - must live within it. */
	_screen.width   = w;
	_screen.height  = amigagfx_game_height();
	_screen.pitch   = amigagfx_pitch();
	_screen.dst_ptr = amigagfx_chunky();

	ResetDirty();

	/* A key held across a resolution change would leave its bit stuck: the
	 * release goes to a window that no longer exists. Same for a pending warp
	 * echo - the new screen will never deliver it. */
	_dirkeys = 0;
	_warp_pending = false;

	BlitterFactoryBase::GetCurrentBlitter()->PostResize();
	UpdatePalette(0, 256);
	GameSizeChanged();

	DEBUG(driver, 1, "amiga: %dx%d, %s", w, h,
	      got_rtg ? "8bpp RTG, chunky straight to the card"
	              : "8 bitplanes, Kalms c2p");
	return true;
}

static void PollEvents()
{
	AmigaGfxEvent ev;

	while (amigagfx_poll(&ev) != 0) {
		switch (ev.type) {
			case AMIGAGFX_EV_MOUSEMOVE:
				/* Echo of our own pointer warp? Swallow it: it is not user
				 * movement. If the drag ended before the echo arrived, fall
				 * through instead - the pointer genuinely is at the warped
				 * position now and the normal path records exactly that.
				 * Moves that do NOT match are pre-warp events still in the
				 * queue and fall through as ordinary deltas; if too many pass
				 * without a match the echo is lost, so unblock and retry. */
				if (_warp_pending) {
					if (_dbg_warp_echo < 8) {
						char b[96];
						snprintf(b, sizeof(b), "move while warp pending: %d,%d (target %d,%d)",
						         ev.x, ev.y, _warp_x, _warp_y);
						amigagfx_log(b);
						_dbg_warp_echo++;
					}
					if (ev.x == _warp_x && ev.y == _warp_y) {
						_warp_pending = false;
						if (_cursor.fix_at) {
							_phys_x = ev.x;
							_phys_y = ev.y;
							break;
						}
					} else if (++_warp_misses >= WARP_ECHO_LIMIT) {
						_warp_pending = false;
						amigagfx_log("warp echo never arrived - unblocking to retry");
					}
				}
				/* Right-button map scrolling: OpenTTD sets _cursor.fix_at and
				 * then expects the driver to pin the pointer and report only
				 * the movement deltas. SDL does that by warping the mouse;
				 * Intuition has no equivalent, so instead we track the physical
				 * position ourselves and leave _cursor.pos anchored. That gives
				 * OpenTTD correct incremental deltas, which is what actually
				 * drives the scroll. Without this branch the map does not
				 * scroll on right-drag at all. */
				if (_cursor.fix_at) {
					_cursor.delta.x = ev.x - _phys_x;
					_cursor.delta.y = ev.y - _phys_y;
					if (_verbose && _dbg_scroll < 8) {
						char b[96];
						snprintf(b, sizeof(b), "scroll delta %d,%d (rmb=%d)",
						         (int)_cursor.delta.x, (int)_cursor.delta.y,
						         (int)_right_button_down);
						amigagfx_log(b);
						_dbg_scroll++;
					}
				} else {
					_cursor.delta.x = ev.x - _cursor.pos.x;
					_cursor.delta.y = ev.y - _cursor.pos.y;
					_cursor.pos.x = ev.x;
					_cursor.pos.y = ev.y;
				}
				_phys_x = ev.x;
				_phys_y = ev.y;
				_cursor.dirty = true;
				_cursor.in_window = true;
				HandleMouseEvents();
				break;

			case AMIGAGFX_EV_MOUSEDOWN:
				if (ev.code == AMIGAGFX_BUTTON_LEFT || _rmb_as_lmb) {
					if (ev.code != AMIGAGFX_BUTTON_LEFT) amigagfx_log("RMB down -> treated as LMB");
					_left_button_down = true;
				} else {
					/* BOTH flags are required. _right_button_down alone only
					 * tells OpenTTD the button is held; it is
					 * _right_button_clicked that window.cpp turns into a
					 * right-click event, and that event is what starts viewport
					 * scrolling. Missing it meant right-drag did nothing at all,
					 * while the button was plainly being received. Both the SDL
					 * and Win32 drivers set the pair together. */
					_right_button_down = true;
					_right_button_clicked = true;
				}
				HandleMouseEvents();
				break;

			case AMIGAGFX_EV_MOUSEUP:
				if (ev.code == AMIGAGFX_BUTTON_LEFT || _rmb_as_lmb) {
					_left_button_down = false;
					_left_button_clicked = false;
				} else {
					_right_button_down = false;
				}
				HandleMouseEvents();
				break;

			case AMIGAGFX_EV_KEY: {
				int  raw  = ev.code & 0x7f;
				bool down = ((ev.code & 0x80) == 0);

				/* _dirkeys is a key-STATE bitmask (1=left 2=up 4=right 8=down)
				 * that every video driver maintains itself; window.cpp polls it
				 * each frame to scroll the viewport. It needs press AND release,
				 * unlike the one-shot dispatch below. */
				switch (raw) {
					case 0x4F: if (down) _dirkeys |= 1; else _dirkeys &= ~1; break; /* left  */
					case 0x4C: if (down) _dirkeys |= 2; else _dirkeys &= ~2; break; /* up    */
					case 0x4E: if (down) _dirkeys |= 4; else _dirkeys &= ~4; break; /* right */
					case 0x4D: if (down) _dirkeys |= 8; else _dirkeys &= ~8; break; /* down  */
					default: break;
				}

				/* Shift and Ctrl arrive as ordinary raw codes with bit 7 set on
				 * release; track them so combinations work. Arrows still go
				 * through HandleAmigaKey too - menus and lists use them as
				 * ordinary keypresses. */
				switch (raw) {
					case 0x60: case 0x61: _amiga_shift = down; break;
					case 0x63:            _amiga_ctrl  = down; break;
					default:
						if (down) HandleAmigaKey(raw);
						break;
				}
				break;
			}

			case AMIGAGFX_EV_QUIT:
				HandleExitGameRequest();
				break;

			default:
				break;
		}
	}

	/* Recentre the system pointer whenever it has moved off the centre during
	 * a fixed-cursor drag - not merely near the edges - so it can never come
	 * anywhere close to a border. Decide only after the queue is drained:
	 * everything still queued carries pre-warp coordinates, and warping
	 * mid-drain would put the echo behind events whose deltas were measured
	 * against the old position. */
	if (_cursor.fix_at && !_warp_pending) {
		int cx = _screen.width  / 2;
		int cy = _screen.height / 2;
		if (_phys_x != cx || _phys_y != cy) {
			if (_dbg_warp_trig < 6) {
				char b[96];
				snprintf(b, sizeof(b), "warp trigger: phys %d,%d -> centre %d,%d (screen %dx%d)",
				         _phys_x, _phys_y, cx, cy, (int)_screen.width, (int)_screen.height);
				amigagfx_log(b);
				_dbg_warp_trig++;
			}
			if (amigagfx_warp_pointer(cx, cy) != 0) {
				_warp_pending = true;
				_warp_misses  = 0;
				_warp_x = cx;
				_warp_y = cy;
			}
		}
	}
}

const char *VideoDriver_Amiga::Start(const char * const *parm)
{
	_force_bbox = (GetDriverParam(parm, "bbox") != NULL);
	_rmb_as_lmb = (GetDriverParam(parm, "rmbaslmb") != NULL);
	_verbose    = (GetDriverParam(parm, "verbose") != NULL);
	amigagfx_set_verbose(_verbose ? 1 : 0);

	{
		const char *fs = GetDriverParam(parm, "frameskip");
		if (fs != NULL) {
			_frameskip_auto = false;
			_frameskip = Clamp(atoi(fs), 0, MAX_FRAMESKIP);
		}
	}

	/* Offer the Amiga modes in Game Options -> Screen resolution: every AGA
	 * mode first, then the RTG modes this machine can actually display. The
	 * order is what the player sees, and AGA-before-RTG keeps the fixed-chipset
	 * modes - the ones every Amiga has - at the top of the list. */
	_num_resolutions = 0;
	uint max_res = lengthof(_resolutions);
	if (max_res > AMIGA_MAX_RESOLUTIONS) max_res = AMIGA_MAX_RESOLUTIONS;

	for (uint i = 0; i < lengthof(_amiga_aga_resolutions) && _num_resolutions < max_res; i++) {
		_amiga_res_rtg[_num_resolutions] = false;
		_resolutions[_num_resolutions++] = _amiga_aga_resolutions[i];
	}

	/* Probed one size at a time rather than "is there a card": a board with
	 * little display memory, or a Picasso96 setup with only a few modes
	 * configured, genuinely may have 640x480 and not 1024x768. Offering a mode
	 * that cannot open would put the player one click away from a screen that
	 * silently falls back to AGA. */
	uint rtg_offered = 0;
	for (uint i = 0; i < lengthof(_amiga_rtg_resolutions) && _num_resolutions < max_res; i++) {
		if (!amigagfx_rtg_has_mode((int)_amiga_rtg_resolutions[i].width,
		                           (int)_amiga_rtg_resolutions[i].height)) continue;
		_amiga_res_rtg[_num_resolutions] = true;
		_resolutions[_num_resolutions++] = _amiga_rtg_resolutions[i];
		rtg_offered++;
	}
	{
		char b[96];
		snprintf(b, sizeof(b), "resolution list: %d AGA + %u RTG mode(s)",
		         (int)lengthof(_amiga_aga_resolutions), rtg_offered);
		amigagfx_log(b);
	}

	/* Which entry is the saved one? _cur_resolution alone cannot say - 640x480
	 * exists on both backends - so the persisted amiga.rtg flag breaks the tie.
	 * Falling back to entry 0 keeps a config written by an older build, or one
	 * naming a mode this machine no longer offers, from selecting nothing. */
	_amiga_cur_res = 0;
	_amiga_pending_res = -1;
	for (uint i = 0; i < _num_resolutions; i++) {
		if (_resolutions[i].width  != _cur_resolution.width)  continue;
		if (_resolutions[i].height != _cur_resolution.height) continue;
		if (_amiga_res_rtg[i] != _settings_client.amiga.rtg)  continue;
		_amiga_cur_res = (int)i;
		break;
	}

	/* Snap to the entry we just settled on. openttd.cfg can easily name a size
	 * that is not in the list - it holds one written by an older build (640x512
	 * has just been retired), one saved on a machine that had RTG modes this one
	 * does not, or one off by a title-bar height. Opening that size anyway would
	 * put a screen on the display that no dropdown entry describes, while the
	 * dropdown confidently showed entry 0 as current. Open what we report. */
	if (_cur_resolution.width  != _resolutions[_amiga_cur_res].width ||
	    _cur_resolution.height != _resolutions[_amiga_cur_res].height) {
		char b[112];
		snprintf(b, sizeof(b), "saved resolution %ux%u is not offered - using %ux%u%s",
		         _cur_resolution.width, _cur_resolution.height,
		         _resolutions[_amiga_cur_res].width, _resolutions[_amiga_cur_res].height,
		         _amiga_res_rtg[_amiga_cur_res] ? " RTG" : " AGA");
		amigagfx_log(b);
		_cur_resolution = _resolutions[_amiga_cur_res];
	}

	/* The font tables are built once, just after this, so the choice has to be
	 * made now. Changing resolution later reopens the screen immediately but
	 * leaves the font as it was - hence the restart note in ChangeResolution. */
	_amiga_small_font = WantSmallFont(_cur_resolution.width);

	if (!CreateMainSurface(_cur_resolution.width, _cur_resolution.height,
	                       _amiga_res_rtg[_amiga_cur_res])) {
		return "Could not open the Amiga screen";
	}

	/* Startup splash: palette-fade the logo in and out on the freshly opened
	 * screen. Deliberately HERE and not in CreateMainSurface, so a later
	 * resolution change never replays it. PROGDIR: finds the file next to the
	 * executable regardless of the current directory. The splash leaves the
	 * palette black, so restore the game palette before anything is drawn. */
	amigagfx_splash("PROGDIR:splash.dat");
	UpdatePalette(0, 256);

	amigagfx_log(_force_bbox
	             ? "dirty tracking: single bounding box (-v amiga:bbox)"
	             : "dirty tracking: rectangle list");
	if (_verbose) amigagfx_log("verbose diagnostics ON (-v amiga:verbose)");

	MarkWholeScreenDirty();
	amigagfx_log("VideoDriver_Amiga::Start done - OpenTTD now loads data");
	return NULL;
}

void VideoDriver_Amiga::Stop()
{
	amigagfx_close();
}

/* cc1plus segfaults optimising this one function (GCC 6.5, m68k, -O1).
 * The build script would otherwise drop the WHOLE file to -O0 - and this
 * file is the per-frame driver: event polling, the blit and the dirty-rect
 * bookkeeping. Losing -O1 across all of it made the game unplayable.
 * So disable optimisation for just the function that trips the bug. */
__attribute__((optimize("O0")))
void VideoDriver_Amiga::MakeDirty(int left, int top, int width, int height)
{
	int right  = left + width;
	int bottom = top + height;

	if (width <= 0 || height <= 0) return;

	/* the bounding box is maintained regardless, as the overflow fallback */
	if (left   < _bbox_left)   _bbox_left   = left;
	if (top    < _bbox_top)    _bbox_top    = top;
	if (right  > _bbox_right)  _bbox_right  = right;
	if (bottom > _bbox_bottom) _bbox_bottom = bottom;

	if (_dirty_overflow || _force_bbox) return;

	/* Merge into an existing rectangle if they touch, then keep merging: one
	 * merge can bring a third rectangle into contact with the result. */
	for (int i = 0; i < _num_dirty; i++) {
		if (!RectsIntersect(&_dirty[i], left, top, right, bottom)) continue;

		if (left   < _dirty[i].left)   _dirty[i].left   = left;
		if (top    < _dirty[i].top)    _dirty[i].top    = top;
		if (right  > _dirty[i].right)  _dirty[i].right  = right;
		if (bottom > _dirty[i].bottom) _dirty[i].bottom = bottom;

		for (int j = _num_dirty - 1; j > i; j--) {
			if (!RectsIntersect(&_dirty[i], _dirty[j].left, _dirty[j].top,
			                    _dirty[j].right, _dirty[j].bottom)) continue;
			if (_dirty[j].left   < _dirty[i].left)   _dirty[i].left   = _dirty[j].left;
			if (_dirty[j].top    < _dirty[i].top)    _dirty[i].top    = _dirty[j].top;
			if (_dirty[j].right  > _dirty[i].right)  _dirty[i].right  = _dirty[j].right;
			if (_dirty[j].bottom > _dirty[i].bottom) _dirty[i].bottom = _dirty[j].bottom;
			/* Split from "_dirty[j] = _dirty[--_num_dirty];": the side effect
			 * inside the subscript segfaults this GCC at -O1 (cc1plus ICE).
			 * Dropping the whole driver to -O0 to dodge it costs real frame
			 * time, so keep the statement split instead. */
			_num_dirty--;
			_dirty[j] = _dirty[_num_dirty];
		}
		return;
	}

	if (_num_dirty >= MAX_DIRTY_RECTS) {
		/* Too fragmented to track individually - one bounding box it is. */
		_dirty_overflow = true;
		return;
	}

	_dirty[_num_dirty].left   = left;
	_dirty[_num_dirty].top    = top;
	_dirty[_num_dirty].right  = right;
	_dirty[_num_dirty].bottom = bottom;
	_num_dirty++;
}

void VideoDriver_Amiga::MainLoop()
{
	uint32 cur_ticks = amigagfx_millis();
	uint32 last_cur_ticks = cur_ticks;
	uint32 next_tick = cur_ticks + 30;
	uint32 pal_tick = 0;
	/* Phase-marker trace of the first three iterations: only under
	 * "-v amiga:verbose". Starting the counter at its limit means not one of
	 * the "if (trace < 3)" comparisons ever logs in normal play. */
	int    trace = _verbose ? 0 : 3;

	amigagfx_log("MainLoop entered - data loaded, game is running");

	for (;;) {
		uint32 prev_cur_ticks = cur_ticks; // to check for wrapping
		if (trace < 3) amigagfx_log("iter: InteractiveRandom");
		InteractiveRandom();

		if (trace < 3) amigagfx_log("iter: PollEvents");
		PollEvents();
		if (_exit_game) break;

		if (trace < 3) amigagfx_log("iter: millis");
		cur_ticks = amigagfx_millis();

		/* How late are we? next_tick is when the current game tick was due, so
		 * anything past it means the previous frame overran its budget. */
		if (_frameskip_auto && !_fast_forward) {
			int32 late = (int32)(cur_ticks - next_tick);
			if (late > 60) {
				if (_frameskip < MAX_FRAMESKIP) _frameskip++;
			} else if (late < 0 && _frameskip > 0) {
				_frameskip--;
			}
		}

		/* Decide before GameLoop so UpdateWindows can be skipped too - that is
		 * where the blitter cost is, and OpenTTD keeps its dirty blocks until
		 * something actually draws them, so nothing is lost by waiting. */
		bool draw_frame = (_skip_left <= 0);
		if (draw_frame) _skip_left = _frameskip; else _skip_left--;

		if (cur_ticks >= next_tick || (_fast_forward && !_pause_mode) || cur_ticks < prev_cur_ticks) {
			_realtime_tick += cur_ticks - last_cur_ticks;
			last_cur_ticks = cur_ticks;
			next_tick = cur_ticks + 30;

			/* Phase markers for the first few iterations only: a crash here
			 * is otherwise indistinguishable between game logic, window
			 * drawing and our own blit. Costs nothing after iteration 3. */
			if (trace < 3) amigagfx_log("GameLoop enter");
			/* OpenTTD throws std::exception from SlError/oldloader and normally
			 * catches it inside SaveOrLoad. Here one escaped all the way to
			 * std::terminate, which either means it came from a path OpenTTD
			 * does not guard, or that stack unwinding is broken in this
			 * libnix/hunk build. Catching it answers that and keeps the game
			 * alive long enough to see the screen. */
			try {
				GameLoop();
			} catch (...) {
				amigagfx_log("EXCEPTION escaped GameLoop - caught, continuing");
			}
			if (trace < 3) amigagfx_log("GameLoop done, UpdateWindows enter");
			UpdateWindows();
			if (trace < 3) amigagfx_log("UpdateWindows done");
			if (++pal_tick > 4) {
				CheckPaletteAnim();
				pal_tick = 1;
			}
		} else {
			if (trace < 3) amigagfx_log("idle: CSleep");
			CSleep(1);
			if (trace < 3) amigagfx_log("idle: NetworkDrawChatMessage");
			NetworkDrawChatMessage();
			if (trace < 3) amigagfx_log("idle: DrawMouseCursor");
			DrawMouseCursor();
			if (trace < 3) amigagfx_log("idle: done");
		}

		if (trace < 3) amigagfx_log("blit enter");
		if (draw_frame) DrawSurfaceToScreen();

		if (_frameskip != _fs_report) {
			char b[80];
			snprintf(b, sizeof(b), "frameskip now %d (drawing 1 frame in %d)",
			         _frameskip, _frameskip + 1);
			amigagfx_log(b);
			_fs_report = _frameskip;
		}
		if (trace < 3) { amigagfx_log("blit done - frame complete"); trace++; }
	}
}

bool VideoDriver_Amiga::ChangeResolution(int w, int h)
{
	/* Apply immediately.
	 *
	 * An earlier version deferred this to a restart, to avoid windows laid out
	 * for the old size ending up off-screen. That was worse: ChangeResInGame()
	 * compares the request against _screen.width/height, so a driver that
	 * changes nothing means _cur_resolution is never updated and the choice is
	 * not even written to openttd.cfg - the player is stuck in both directions.
	 * DELETE closes all windows, which deals with anything left hanging off the
	 * edge. */
	{
		/* Diagnostic, and deliberately in THIS file: if the resolution dropdown
		 * appears to do nothing, this line separates "the handler never ran"
		 * from "the screen could not be reopened". Instrumenting the caller
		 * would risk changing its codegen, which is exactly the failure mode
		 * under suspicion. */
		char b[96];
		snprintf(b, sizeof(b), "ChangeResolution asked for %dx%d (screen is %dx%d)",
		         w, h, _screen.width, _screen.height);
		amigagfx_log(b);
	}

	/* Resolve the backend. Normally the dropdown has just told us the exact
	 * entry (AmigaSetResolutionIndex), which is the only unambiguous answer -
	 * 640x480 appears once as AGA and once as RTG. Anything else that calls
	 * ChangeResolution (the console, a future caller) supplies only a size, so
	 * fall back to a search by size, preferring an entry on the backend already
	 * running, and finally to whatever is on screen now. */
	int index = _amiga_pending_res;
	_amiga_pending_res = -1;
	if (index < 0 || index >= (int)_num_resolutions) {
		index = _amiga_cur_res;
		for (uint i = 0; i < _num_resolutions; i++) {
			if ((int)_resolutions[i].width != w || (int)_resolutions[i].height != h) continue;
			index = (int)i;
			if (_amiga_res_rtg[i] == (amigagfx_backend() == AMIGAGFX_BACKEND_RTG)) break;
		}
	}

	if (!CreateMainSurface(w, h, _amiga_res_rtg[index])) {
		amigagfx_log("ChangeResolution FAILED - screen not reopened");
		return false;
	}
	/* Only now is the entry live; if the open had failed the old one still is.
	 * Note CreateMainSurface has already corrected amiga.rtg if RTG was asked
	 * for and AGA came back, so the persisted flag matches what is on screen. */
	_amiga_cur_res = index;

	/* Store what was actually opened, not what was asked for: the width is
	 * rounded up to the 32-pixel grid the c2p requires. The height must be
	 * the SCREEN height, not _screen.height: with the Workbench bar visible
	 * the game area is shorter, and persisting that would shrink the screen
	 * by one bar height on every restart. */
	_cur_resolution.width  = _screen.width;
	_cur_resolution.height = _amiga_scr_h;

	/* Persist the choice right away. Upstream writes openttd.cfg only on a
	 * clean game exit (the sole SaveToConfig() call site, openttd.cpp), but an
	 * Amiga user typically resets the machine instead - so without this the
	 * new resolution would be lost on every hard reset. */
	SaveToConfig();

	/* The font still cannot follow - glyph tables and window widths are built
	 * once at startup - so say so instead of leaving a lores screen wearing the
	 * hires font. */
	if (WantSmallFont((uint)_screen.width) != _amiga_small_font) {
		amigagfx_log(WantSmallFont((uint)_screen.width)
		             ? "lores: restart for the small interface font"
		             : "hires: restart for the normal interface font");
	}
	return true;
}

/**
 * The "amiga.wb_bar" setting was toggled (called from the setting's proc in
 * settings.cpp - which must not know any Amiga types, hence this plain
 * function rather than anything driver-shaped). The flag is read when the
 * screen is opened, so apply it exactly like a resolution change: tear the
 * screen down and reopen it at the same SCREEN size; CreateMainSurface then
 * derives the new game area from what Intuition reports.
 */
void AmigaWorkbenchBarChanged()
{
	/* The console can flip settings before/without a video driver; nothing to
	 * reopen then - the value is simply picked up when the screen opens. */
	if (_amiga_scr_w == 0) return;

	{
		char b[96];
		snprintf(b, sizeof(b), "wb_bar now %d - reopening %ux%u screen",
		         _settings_client.amiga.wb_bar ? 1 : 0, _amiga_scr_w, _amiga_scr_h);
		amigagfx_log(b);
	}

	/* Same backend as the screen being replaced - a bar toggle must not quietly
	 * move the player between AGA and RTG. */
	if (!CreateMainSurface(_amiga_scr_w, _amiga_scr_h, _amiga_res_rtg[_amiga_cur_res])) {
		amigagfx_log("wb_bar toggle FAILED - screen not reopened");
		return;
	}

	/* Same reasoning as ChangeResolution: an Amiga user resets the machine
	 * instead of quitting cleanly, so persist the choice right away. */
	SaveToConfig();
}

/* ---- resolution-list hooks called from generic code ----------------------
 *
 * Three tiny functions, declared with a local "extern" at each call site the
 * way AmigaWorkbenchBarChanged() already is, so no generic header has to learn
 * about the Amiga. They exist for one reason: _resolutions[] holds only a width
 * and a height, and 640x480 is offered on both AGA and RTG, so a size is not an
 * identity. The dropdown INDEX is, and these carry it across.
 */

/** settings_gui.cpp, OnDropdownSelect: the entry the player just picked, told
 * to us before ChangeResInGame() so ChangeResolution() knows which backend the
 * chosen 640x480 means. */
void AmigaSetResolutionIndex(int index)
{
	_amiga_pending_res = (index >= 0 && index < (int)_num_resolutions) ? index : -1;
}

/** settings_gui.cpp, GetCurRes(): the entry actually on screen. Upstream finds
 * this by searching _resolutions[] for a size match, which on an RTG machine
 * always returns the AGA 640x480 and would mislabel an RTG screen. */
int AmigaGetResolutionIndex()
{
	return _amiga_cur_res;
}

/** strings.cpp, the SPECSTR_RESOLUTION_* formatter: the backend suffix for one
 * entry. This is the single formatter behind both the closed dropdown label and
 * the open dropdown list, so every place a mode is named gets the same answer
 * and AGA and RTG modes are never mistaken for one another. */
const char *AmigaResolutionSuffix(int index)
{
	if (index < 0 || index >= (int)_num_resolutions) return "";
	return _amiga_res_rtg[index] ? " RTG" : " AGA";
}

bool VideoDriver_Amiga::ToggleFullscreen(bool fullscreen)
{
	/* An Intuition custom screen is always "fullscreen"; there is no windowed
	 * mode to switch to, so report success only for the state we are in. */
	return fullscreen;
}
