/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS music driver for OpenTTD 1.0.5: streams IMA-ADPCM WAV from
 * disk and plays it on Paula channels 2 (right) + 3 (left) via amiga_audio.c.
 * The title theme plays in the menu; the in-game tracks rotate during play.
 * All decoding/streaming is in amiga_adpcm.c; all Paula I/O in amiga_audio.c.
 * See amiga_m.cpp for the design. */

#ifndef MUSIC_AMIGA_H
#define MUSIC_AMIGA_H

#include "music_driver.hpp"

class MusicDriver_Amiga: public MusicDriver {
public:
	/* virtual */ const char *Start(const char * const *param);
	/* virtual */ void Stop();
	/* virtual */ void PlaySong(const char *filename);
	/* virtual */ void StopSong();
	/* virtual */ bool IsSongPlaying();
	/* virtual */ void SetVolume(byte vol);
	/* virtual */ const char *GetName() const { return "amiga"; }
};

class FMusicDriver_Amiga: public MusicDriverFactory<FMusicDriver_Amiga> {
public:
	/* Must outrank "null" (1) so it is chosen automatically on Amiga. */
	static const int priority = 10;
	/* virtual */ const char *GetName() { return "amiga"; }
	/* virtual */ const char *GetDescription() { return "Amiga Paula Music Driver"; }
	/* virtual */ Driver *CreateInstance() { return new MusicDriver_Amiga(); }
};

#endif /* MUSIC_AMIGA_H */
