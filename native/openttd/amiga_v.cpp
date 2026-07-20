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
#include "../openttd.h"
#include "../gfx_func.h"
#include "../variables.h"
#include "../rev.h"
#include "../blitter/factory.hpp"
#include "../network/network.h"
#include "../functions.h"
#include "../genworld.h"
#include "../core/random_func.hpp"
#include "amiga_v.h"
#include "amiga_gfx.h"

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
		DrawSurfaceToScreen();
		if (trace < 3) { amigagfx_log("blit done - frame complete"); trace++; }
	}
}

bool VideoDriver_Amiga::ChangeResolution(int w, int h)
{
	return CreateMainSurface(w, h);
}

bool VideoDriver_Amiga::ToggleFullscreen(bool fullscreen)
{
	/* An Intuition custom screen is always "fullscreen"; there is no windowed
	 * mode to switch to, so report success only for the state we are in. */
	return fullscreen;
}
