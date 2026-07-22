/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS music driver for OpenTTD 1.0.5.
 *
 * The game's music set (gm/*.gm) exists only to make OpenTTD's music manager
 * populate a playlist and call PlaySong()/IsSongPlaying(); we ignore the .gm
 * filename it hands us and instead play our own IMA-ADPCM WAVs:
 *   - in the menu (GM_MENU): the title theme, PROGDIR:music/Title/*.wav;
 *   - in game: the 15 tracks in PROGDIR:music/Nowe/, rotating one per call.
 *
 * Streaming, not loading: amiga_adpcm.c keeps the WAV open and decodes it
 * block-by-block from a 256 KB disk staging buffer; amiga_audio.c plays the
 * decoded 8-bit stream on Paula channels 2 (right) + 3 (left) with gapless
 * double buffering, refilled once per frame from the video main loop's call
 * to AmigaAudio_MusicService(). While music streams the SFX side confines
 * itself to channels 0 and 1; with music stopped it uses all four.
 *
 * A track is played once; when it drains, IsSongPlaying() returns false and
 * OpenTTD's MusicLoop advances to the next song (in game) or replays the
 * theme (in menu). Switching between menu and game is made prompt by
 * reporting "not playing" as soon as the game mode no longer matches the
 * track that is loaded.
 *
 * This file uses only the plain-C amiga_audio.h / amiga_adpcm.h APIs and the
 * C stdio fopen() logger, never any <proto/*> header, so it does not hit the
 * OTTD_Point collision that forces amiga_audio.c to stay in C.
 */

#include "../stdafx.h"
#include "../openttd.h"     /* _game_mode, GM_MENU */
#include "amiga_m.h"

extern "C" {
#include "../sound/amiga_audio.h"
#include "../sound/amiga_adpcm.h"
}

#include <stdio.h>
#include <stdarg.h>

/** Factory for the Amiga Paula music driver. */
static FMusicDriver_Amiga iFMusicDriver_Amiga;

/* PAL Paula clock: sample rate = PAL_CLOCK / period. */
static const unsigned long MUS_PAL_CLOCK = 3546895UL;
/* 8-bit samples per Paula buffer (~0.37 s at 22050 Hz). Two are kept queued. */
static const int MUS_CHUNK = 8192;

/* PROGDIR: resolves to the directory the binary was loaded from (Work:), the
 * same anchor amiga_snd.log uses. The old WAVs (music/Stare) are deliberately
 * NOT referenced or shipped. */
static const char *TITLE_TRACK = "PROGDIR:music/Title/AmigaTTD Theme.wav";
static const char * const GAME_TRACKS[] = {
	"PROGDIR:music/Nowe/Coast and Roll.wav",
	"PROGDIR:music/Nowe/Cruising Gear Two.wav",
	"PROGDIR:music/Nowe/Downtown Traffic Funk.wav",
	"PROGDIR:music/Nowe/Fast Lane Fever.wav",
	"PROGDIR:music/Nowe/Grand Terminal Anthem.wav",
	"PROGDIR:music/Nowe/Highway Cruise Overdrive.wav",
	"PROGDIR:music/Nowe/Idle Engine Chill.wav",
	"PROGDIR:music/Nowe/Neon Crosswalk Strut.wav",
	"PROGDIR:music/Nowe/Open Road Cruise.wav",
	"PROGDIR:music/Nowe/Rush Hour Swagger.wav",
	"PROGDIR:music/Nowe/Skyline Express Beat.wav",
	"PROGDIR:music/Nowe/Smooth Motorway Glide.wav",
	"PROGDIR:music/Nowe/Sunday Driver Groove.wav",
	"PROGDIR:music/Nowe/Turbo Lane Boogie.wav",
	"PROGDIR:music/Nowe/Tycoon Skyline Theme.wav",
};
static const int NUM_GAME_TRACKS = (int)(sizeof(GAME_TRACKS) / sizeof(GAME_TRACKS[0]));

/* Song list shown in the music window and used to synthesise the base music
 * set (see music.cpp): index 0 is the theme (song 1 in game), 1..15 are the
 * in-game tracks (songs 2..16). Order matches GAME_TRACKS so song number
 * cursong maps to GAME_TRACKS[cursong - 2]. */
static const char * const SONG_NAMES[] = {
	"AmigaTTD Theme",
	"Coast and Roll",
	"Cruising Gear Two",
	"Downtown Traffic Funk",
	"Fast Lane Fever",
	"Grand Terminal Anthem",
	"Highway Cruise Overdrive",
	"Idle Engine Chill",
	"Neon Crosswalk Strut",
	"Open Road Cruise",
	"Rush Hour Swagger",
	"Skyline Express Beat",
	"Smooth Motorway Glide",
	"Sunday Driver Groove",
	"Turbo Lane Boogie",
	"Tycoon Skyline Theme",
};
static const int NUM_SONGS = (int)(sizeof(SONG_NAMES) / sizeof(SONG_NAMES[0]));

/* Consumed by music.cpp's synthesised MusicSet::FillSetDetails. */
extern "C" int AmigaMusic_NumSongs(void) { return NUM_SONGS; }
extern "C" const char *AmigaMusic_SongName(int idx)
{
	if (idx < 0 || idx >= NUM_SONGS) return "";
	return SONG_NAMES[idx];
}

/* OpenTTD's currently selected song number (1-based; 1 = theme). Set by the
 * music manager just before PlaySong(); we resolve the WAV from it. */
extern byte _music_wnd_cursong;

static AdpcmStream *_cur_stream = NULL;
static bool  _cur_is_menu = false;   /* true while the loaded track is the theme */
static byte  _cur_vol     = 127;     /* OpenTTD music volume 0..127 */

#define MUS_LOG "PROGDIR:amiga_mus.log"
static int  _mus_log_lines = 0;
static void MusLog(const char *fmt, ...)
{
	if (_mus_log_lines >= 60) return;   /* capped, one-run-conclusive */
	_mus_log_lines++;
	char buf[192];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	FILE *f = fopen(MUS_LOG, "a");
	if (f == NULL) return;
	fputs(buf, f); fputc('\n', f);
	fclose(f);
}

/* Pull callback for amiga_audio.c: decode more 8-bit samples from the WAV. */
extern "C" int AmigaMusicRefill(void *ud, signed char *dst, int max)
{
	AdpcmStream *s = (AdpcmStream *)ud;
	return (s != NULL) ? Adpcm_Decode(s, dst, max) : 0;
}

static int VolToPaula(byte vol) { return (int)(vol >> 1); }   /* 0..127 -> 0..63 */

const char *MusicDriver_Amiga::Start(const char * const *param)
{
	/* The sound driver normally opens Paula, but open here too so music works
	 * even with a different sound driver; AmigaAudio_Open is idempotent. */
	AmigaAudio_Open();
	remove(MUS_LOG);
	MusLog("AMIGA-MUSIC-v1 start: %d game tracks", NUM_GAME_TRACKS);
	return NULL;   /* never fail: a failed explicit driver is a fatal usererror */
}

void MusicDriver_Amiga::Stop()
{
	this->StopSong();
}

void MusicDriver_Amiga::PlaySong(const char *filename)
{
	this->StopSong();

	/* Resolve the WAV from the selected song number: 1 = theme, 2..16 = the
	 * in-game tracks. In the menu OpenTTD forces song 1 (theme). */
	int cs = (int)_music_wnd_cursong;
	const char *path;
	if (cs <= 1) {
		path = TITLE_TRACK;
		_cur_is_menu = true;
	} else {
		path = GAME_TRACKS[(cs - 2) % NUM_GAME_TRACKS];
		_cur_is_menu = false;
	}

	_cur_stream = Adpcm_Open(path);
	if (_cur_stream == NULL) {
		MusLog("open FAILED: %s", path);
		return;   /* IsSongPlaying() will report false and the game advances */
	}

	int rate = Adpcm_Rate(_cur_stream);
	if (rate <= 0) rate = 22050;
	int period = (int)(MUS_PAL_CLOCK / (unsigned long)rate);

	AmigaAudio_MusicSetVolume(VolToPaula(_cur_vol));
	if (!AmigaAudio_MusicStart(period, MUS_CHUNK, AmigaMusicRefill, _cur_stream)) {
		MusLog("MusicStart FAILED: %s", path);
		Adpcm_Close(_cur_stream);
		_cur_stream = NULL;
		return;
	}
	MusLog("play song %d -> %s (rate=%d period=%d menu=%d)", cs, path, rate, period, _cur_is_menu ? 1 : 0);
	(void)filename;   /* OpenTTD's .gm path is ignored; we stream our own WAV */
}

void MusicDriver_Amiga::StopSong()
{
	AmigaAudio_MusicStop();
	if (_cur_stream != NULL) {
		Adpcm_Close(_cur_stream);
		_cur_stream = NULL;
	}
}

bool MusicDriver_Amiga::IsSongPlaying()
{
	if (_cur_stream == NULL) return false;
	/* Switch promptly when crossing the menu<->game boundary. */
	if (_cur_is_menu != (_game_mode == GM_MENU)) return false;
	return AmigaAudio_MusicFinished() ? false : true;
}

void MusicDriver_Amiga::SetVolume(byte vol)
{
	_cur_vol = vol;
	AmigaAudio_MusicSetVolume(VolToPaula(vol));
}
