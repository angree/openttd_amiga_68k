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
#include <stdio.h>      /* snprintf for the blit statistics line */
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
#include "amiga_v.h"
#include "amiga_gfx.h"

/**
 * The modes offered in Game Options -> Screen resolution.
 *
 * Widths are all multiples of 32 because Kalms' c2p converts 32-pixel columns;
 * 368, the other common PAL overscan width, is not and would be rejected.
 *
 *   320x256  plain PAL lores, no interlace - no flicker
 *   352x272  PAL lores with overscan: same mode, bigger window
 *   640x480  PAL hires interlaced - the readable, default choice
 *   640x512  full PAL hires interlaced
 */
static const Dimension _amiga_resolutions[] = {
	{ 320, 256 },
	{ 352, 272 },
	{ 640, 480 },
	{ 640, 512 },
};


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

/** Proof that the rectangle list is worth it: how many pixels we actually
 * convert versus what a single bounding box would have cost. */
static uint32 _stat_frames, _stat_px_real, _stat_px_bbox;

static void ReportBlitStats()
{
	char buf[128];
	uint32 saved = (_stat_px_bbox > _stat_px_real)
	             ? ((_stat_px_bbox - _stat_px_real) * 100 / _stat_px_bbox) : 0;
	snprintf(buf, sizeof(buf),
	         "%u frames: %u kpx converted vs %u kpx for one bbox (%u%% saved)",
	         (uint)_stat_frames, (uint)(_stat_px_real / 1000),
	         (uint)(_stat_px_bbox / 1000), (uint)saved);
	amigagfx_log(buf);
	_stat_frames = 0;
	_stat_px_real = 0;
	_stat_px_bbox = 0;
}

static void DrawSurfaceToScreen()
{
	if (_bbox_right > _bbox_left && _bbox_bottom > _bbox_top) {
		_stat_px_bbox += (uint32)(_bbox_right - _bbox_left) * (_bbox_bottom - _bbox_top);
		if (_dirty_overflow || _force_bbox) {
			_stat_px_real += (uint32)(_bbox_right - _bbox_left) * (_bbox_bottom - _bbox_top);
		} else {
			for (int i = 0; i < _num_dirty; i++) {
				_stat_px_real += (uint32)(_dirty[i].right - _dirty[i].left) *
				                 (_dirty[i].bottom - _dirty[i].top);
			}
		}
		if (++_stat_frames >= 200) ReportBlitStats();
	}

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

static bool CreateMainSurface(uint w, uint h)
{
	/* The c2p converts 32 pixels at a time, so the width must be a multiple
	 * of 32. Round up rather than fail; 640 and 800 already qualify. */
	w = (w + 31) & ~31;

	amigagfx_close();
	int err = amigagfx_open((int)w, (int)h);
	if (err != 0) {
		DEBUG(driver, 0, "amiga: could not open a %dx%d 256-colour screen (error %d)", w, h, err);
		return false;
	}

	_screen.width   = w;
	_screen.height  = h;
	_screen.pitch   = amigagfx_pitch();
	_screen.dst_ptr = amigagfx_chunky();

	ResetDirty();

	BlitterFactoryBase::GetCurrentBlitter()->PostResize();
	UpdatePalette(0, 256);
	GameSizeChanged();

	DEBUG(driver, 1, "amiga: %dx%d, 8 bitplanes, Kalms c2p", w, h);
	return true;
}

static void PollEvents()
{
	AmigaGfxEvent ev;

	while (amigagfx_poll(&ev) != 0) {
		switch (ev.type) {
			case AMIGAGFX_EV_MOUSEMOVE:
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
					if (_dbg_scroll < 8) {
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

			case AMIGAGFX_EV_KEY:
				/* Shift and Ctrl arrive as ordinary raw codes with bit 7 set on
				 * release; track them so combinations work. */
				switch (ev.code & 0x7f) {
					case 0x60: case 0x61: _amiga_shift = ((ev.code & 0x80) == 0); break;
					case 0x63:            _amiga_ctrl  = ((ev.code & 0x80) == 0); break;
					default:
						if ((ev.code & 0x80) == 0) HandleAmigaKey(ev.code & 0x7f);
						break;
				}
				break;

			case AMIGAGFX_EV_QUIT:
				HandleExitGameRequest();
				break;

			default:
				break;
		}
	}
}

const char *VideoDriver_Amiga::Start(const char * const *parm)
{
	_force_bbox = (GetDriverParam(parm, "bbox") != NULL);
	_rmb_as_lmb = (GetDriverParam(parm, "rmbaslmb") != NULL);

	{
		const char *fs = GetDriverParam(parm, "frameskip");
		if (fs != NULL) {
			_frameskip_auto = false;
			_frameskip = Clamp(atoi(fs), 0, MAX_FRAMESKIP);
		}
	}

	/* Offer the Amiga modes in Game Options -> Screen resolution. */
	_num_resolutions = 0;
	for (uint i = 0; i < lengthof(_amiga_resolutions) && _num_resolutions < lengthof(_resolutions); i++) {
		_resolutions[_num_resolutions++] = _amiga_resolutions[i];
	}

	/* The font tables are built once, just after this, so the choice has to be
	 * made now. Changing resolution later reopens the screen immediately but
	 * leaves the font as it was - hence the restart note in ChangeResolution. */
	_amiga_small_font = WantSmallFont(_cur_resolution.width);

	if (!CreateMainSurface(_cur_resolution.width, _cur_resolution.height)) {
		return "Could not open the Amiga screen";
	}

	amigagfx_log(_force_bbox
	             ? "dirty tracking: single bounding box (-v amiga:bbox)"
	             : "dirty tracking: rectangle list");

	MarkWholeScreenDirty();
	amigagfx_log("VideoDriver_Amiga::Start done - OpenTTD now loads data");
	return NULL;
}

void VideoDriver_Amiga::Stop()
{
	amigagfx_close();
}

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
			_dirty[j] = _dirty[--_num_dirty];
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
	int    trace = 0;

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
	/* Deliberately does NOT reopen the screen.
	 *
	 * Switching size on the fly leaves the interface built for the old one:
	 * every open window keeps its position and width, so going from 640 to 320
	 * left a single window covering the whole display. The font cannot follow
	 * either - the glyph tables are built once at startup. Changing both
	 * properly means re-laying out every window and rebuilding the fonts, which
	 * is a far bigger job than it looks.
	 *
	 * So the choice is recorded and applied on the next run. Returning true is
	 * what makes OpenTTD keep the new value and write it to openttd.cfg. */
	amigagfx_log(WantSmallFont((uint)w)
	             ? "resolution set - restart to apply (lores, small interface font)"
	             : "resolution set - restart to apply (hires, normal interface font)");
	return true;
}

bool VideoDriver_Amiga::ToggleFullscreen(bool fullscreen)
{
	/* An Intuition custom screen is always "fullscreen"; there is no windowed
	 * mode to switch to, so report success only for the state we are in. */
	return fullscreen;
}
