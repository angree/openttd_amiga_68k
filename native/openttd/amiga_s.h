/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS sound driver for OpenTTD 1.0.5: per-effect Paula playback.
 *
 * No software mixing. Each sound effect is converted once to 8-bit signed
 * at <= ~28.6 kHz, cached in Chip RAM and handed to one of the four Paula
 * channels, which plays it by DMA - the CPU does nothing after the write is
 * started. A fifth simultaneous sound is simply dropped (accepted design).
 * The companion mixer implementation lives in amiga_s.cpp and REPLACES
 * mixer.cpp in the build; all OS calls live in amiga_audio.c (plain C).
 */

#ifndef SOUND_AMIGA_H
#define SOUND_AMIGA_H

#include "sound_driver.hpp"

class SoundDriver_Amiga: public SoundDriver {
public:
	/* virtual */ const char *Start(const char * const *param);

	/* virtual */ void Stop();

	/* Reaps finished Paula writes once per game tick (also in menu mode)
	 * and emits the capped diagnostic snapshots. */
	/* virtual */ void MainLoop();

	/* virtual */ const char *GetName() const { return "amiga"; }
};

class FSoundDriver_Amiga: public SoundDriverFactory<FSoundDriver_Amiga> {
public:
	/* Must outrank "null" (1) so it is chosen automatically on Amiga. */
	static const int priority = 10;
	/* virtual */ const char *GetName() { return "amiga"; }
	/* virtual */ const char *GetDescription() { return "Amiga Paula Sound Driver"; }
	/* virtual */ Driver *CreateInstance() { return new SoundDriver_Amiga(); }
};

#endif /* SOUND_AMIGA_H */
