/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS video driver for OpenTTD 1.0.5 (AGA / CyberGraphX-free).
 *
 * Renders into a chunky 8bpp buffer in Fast RAM - which is exactly what
 * OpenTTD's 8bpp blitter already produces - and pushes dirty rectangles to a
 * planar AGA screen with Mikael Kalms' 68040 chunky-to-planar routine.
 */

#ifndef VIDEO_AMIGA_H
#define VIDEO_AMIGA_H

#include "video_driver.hpp"

class VideoDriver_Amiga: public VideoDriver {
public:
	/* virtual */ const char *Start(const char * const *param);

	/* virtual */ void Stop();

	/* virtual */ void MakeDirty(int left, int top, int width, int height);

	/* virtual */ void MainLoop();

	/* virtual */ bool ChangeResolution(int w, int h);

	/* virtual */ bool ToggleFullscreen(bool fullscreen);

	/* virtual */ const char *GetName() const { return "amiga"; }
};

class FVideoDriver_Amiga: public VideoDriverFactory<FVideoDriver_Amiga> {
public:
	/* Must outrank "null" (0) so it is chosen automatically on Amiga. */
	static const int priority = 10;
	/* virtual */ const char *GetName() { return "amiga"; }
	/* virtual */ const char *GetDescription() { return "Amiga AGA Video Driver"; }
	/* virtual */ Driver *CreateInstance() { return new VideoDriver_Amiga(); }
};

#endif /* VIDEO_AMIGA_H */
