/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 */

/* Native AmigaOS music driver for OpenTTD 1.0.5.
 *
 * amiga_mscan.c scans the four music directories at startup. It is a separate
 * plain-C translation unit because AmigaOS <proto/*> headers collide with an
 * OpenTTD C++ Point declaration; this C++ file never includes those headers.
 *
 * DESIGN: song number IS the track. The scanned files form one ordered
 * catalogue and OpenTTD's song number (_music_wnd_cursong, 1-based) indexes it
 * directly, so the name the music window shows is exactly the WAV that plays,
 * and Next/Prev/Shuffle (OpenTTD owns the playlist and its shuffle) all work.
 *
 *   track 1               = the menu theme (music/Title, or a fallback)
 *   tracks 2..1+N         = the in-game set: music/Nowe then music/Extra  (N)
 *   tracks 2+N..1+N+O     = music/Stare                                   (O)
 *
 * music_gui.cpp's InitializeMusic() builds the playlists from N and O
 * (AmigaMusic_NormalCount / AmigaMusic_OldCount): All/New/Ezy = the in-game
 * set, Old Style = Stare. The year and OpenTTD's era classes are ignored.
 *
 * Streaming stays in amiga_adpcm.c / amiga_audio.c; this file uses only their
 * plain-C APIs, the scanner API, stdio logging and OpenTTD state.
 */

#include "../stdafx.h"
#include "../openttd.h"                 /* _game_mode, GM_MENU */
#include "amiga_m.h"

extern "C" {
#include "../sound/amiga_audio.h"
#include "../sound/amiga_adpcm.h"
#include "amiga_mscan.h"
}

/* OpenTTD's current 1-based song number, exported by music_gui.cpp. */
extern byte _music_wnd_cursong;

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/** Factory for the Amiga Paula music driver. */
static FMusicDriver_Amiga iFMusicDriver_Amiga;

/* PAL Paula clock: sample rate = PAL_CLOCK / period. */
static const unsigned long MUS_PAL_CLOCK = 3546895UL;
/* 8-bit samples per Paula buffer (~0.37 s at 22050 Hz). */
static const int MUS_CHUNK = 8192;

/* Per-directory disk enumeration bound (Nowe/Extra/Stare can each be large). */
static const int MUS_MAX_DIR_TRACKS = 128;
/* Song limit. Song numbers are a byte in OpenTTD, so 255 is the hard ceiling;
 * NUM_SONGS_AVAILABLE is 256 (array size) but we never issue number 256. */
static const int MUS_MAX_SONGS = 255;

/* Temporary per-directory scan results. */
static char _dir_paths[MUS_MAX_DIR_TRACKS][AMIGA_MUSIC_PATH_MAX];
static char _dir_names[MUS_MAX_DIR_TRACKS][AMIGA_MUSIC_NAME_MAX];

/* The one ordered catalogue: index i == song number i+1. */
static char _cat_path[MUS_MAX_SONGS][AMIGA_MUSIC_PATH_MAX];
static char _cat_name[MUS_MAX_SONGS][AMIGA_MUSIC_NAME_MAX];
static int  _song_count  = 0;   /* total catalogue entries (theme + normal + old) */
static int  _normal_count = 0;  /* Nowe + Extra (the in-game default set) */
static int  _old_count    = 0;  /* Stare */
static bool _scan_done    = false;

static AdpcmStream *_cur_stream = NULL;
static bool  _cur_is_menu = false;   /* game mode the loaded track was chosen for */
static int   _cur_song    = 0;       /* song number of the loaded track */
static byte  _cur_vol     = 127;

#define MUS_LOG "PROGDIR:amiga_mus.log"
static int _mus_log_lines = 0;
static void MusLog(const char *fmt, ...)
{
	char buf[192];
	va_list va;
	FILE *f;
	if (_mus_log_lines >= 60) return;
	_mus_log_lines++;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	f = fopen(MUS_LOG, "a");
	if (f == NULL) return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}

/* Append one catalogue entry; returns true if it was actually added. */
static bool CatAdd(const char *path, const char *name)
{
	if (_song_count >= MUS_MAX_SONGS) return false;
	snprintf(_cat_path[_song_count], AMIGA_MUSIC_PATH_MAX, "%s", path);
	snprintf(_cat_name[_song_count], AMIGA_MUSIC_NAME_MAX, "%s", name);
	_song_count++;
	return true;
}

static void ScanMusicDirectories()
{
	int n, i;

	_song_count = 0;
	_normal_count = 0;
	_old_count = 0;

	/* Track 1: the menu theme. Prefer music/Title; fall back to the first
	 * in-game track, or a placeholder so song 1 always exists. */
	n = AmigaMusic_ScanDir("PROGDIR:music/Title", _dir_paths, _dir_names, MUS_MAX_DIR_TRACKS);
	int title_n = n;
	if (n > 0) {
		CatAdd(_dir_paths[0], _dir_names[0]);
	} else {
		/* filled in after Nowe is scanned, below; keep slot 0 reserved. */
		CatAdd("", "Amiga music");
	}

	/* Tracks 2..1+N: the in-game set = Nowe then Extra. */
	n = AmigaMusic_ScanDir("PROGDIR:music/Nowe", _dir_paths, _dir_names, MUS_MAX_DIR_TRACKS);
	int nowe_n = n;
	for (i = 0; i < n; i++) {
		if (CatAdd(_dir_paths[i], _dir_names[i])) _normal_count++;
	}
	/* If there was no Title theme, use the first Nowe track for the menu. */
	if (title_n == 0 && nowe_n > 0) {
		snprintf(_cat_path[0], AMIGA_MUSIC_PATH_MAX, "%s", _dir_paths[0]);
		snprintf(_cat_name[0], AMIGA_MUSIC_NAME_MAX, "%s", _dir_names[0]);
	}

	int extra_n = 0;
	if (nowe_n > 0) {   /* Extra extends Nowe; ignored when Nowe is empty. */
		n = AmigaMusic_ScanDir("PROGDIR:music/Extra", _dir_paths, _dir_names, MUS_MAX_DIR_TRACKS);
		extra_n = n;
		for (i = 0; i < n; i++) {
			if (CatAdd(_dir_paths[i], _dir_names[i])) _normal_count++;
		}
	}

	/* Tracks 2+N..1+N+O: Stare (played only via the Old Style playlist). */
	n = AmigaMusic_ScanDir("PROGDIR:music/Stare", _dir_paths, _dir_names, MUS_MAX_DIR_TRACKS);
	int stare_n = n;
	for (i = 0; i < n; i++) {
		if (CatAdd(_dir_paths[i], _dir_names[i])) _old_count++;
	}

	_scan_done = true;
	MusLog("scan: Title=%d Nowe=%d Extra=%d Stare=%d -> songs=%d (normal=%d old=%d)",
			title_n, nowe_n, extra_n, stare_n, _song_count, _normal_count, _old_count);
	if (nowe_n == 0 && extra_n > 0) MusLog("Extra ignored: Nowe empty");
}

static void EnsureScanned()
{
	if (!_scan_done) ScanMusicDirectories();
}

/* --- consumed by music.cpp (song catalogue) and music_gui.cpp (playlists) --- */
extern "C" int AmigaMusic_NumSongs(void)   { EnsureScanned(); return _song_count; }
extern "C" const char *AmigaMusic_SongName(int idx)
{
	EnsureScanned();
	if (idx < 0 || idx >= _song_count) return "";
	return _cat_name[idx];
}
/* How many in-game (Nowe+Extra) and Old (Stare) tracks exist, so InitializeMusic
 * can lay the playlists out over the right song numbers. */
extern "C" int AmigaMusic_NormalCount(void) { EnsureScanned(); return _normal_count; }
extern "C" int AmigaMusic_OldCount(void)    { EnsureScanned(); return _old_count; }

/* Pull callback for amiga_audio.c. */
extern "C" int AmigaMusicRefill(void *ud, signed char *dst, int max)
{
	AdpcmStream *s = (AdpcmStream *)ud;
	return s != NULL ? Adpcm_Decode(s, dst, max) : 0;
}

static int VolToPaula(byte vol) { return (int)(vol >> 1); }   /* 0..127 -> 0..63 */

const char *MusicDriver_Amiga::Start(const char * const *param)
{
	(void)param;
	AmigaAudio_Open();
	remove(MUS_LOG);
	_mus_log_lines = 0;
	ScanMusicDirectories();
	MusLog("AMIGA-MUSIC-v3 (song number = track) started");
	return NULL;   /* never fail: an explicit driver failure is fatal upstream */
}

void MusicDriver_Amiga::Stop() { this->StopSong(); }

void MusicDriver_Amiga::PlaySong(const char *filename)
{
	int cs;
	const char *path;
	int rate, period;

	this->StopSong();
	EnsureScanned();

	/* The title screen ALWAYS plays the menu theme (song 1 = music/Title),
	 * never an in-game track, whatever song number OpenTTD left selected. In
	 * game, play exactly the track OpenTTD picked: song number cs -> catalogue
	 * entry cs-1, so the name the window shows is the WAV that plays. */
	if (_game_mode == GM_MENU) {
		cs = 1;
	} else {
		cs = (int)_music_wnd_cursong;
		if (cs < 1 || cs > _song_count) cs = 1;
	}
	path = (_song_count > 0) ? _cat_path[cs - 1] : "";

	_cur_song = cs;
	_cur_is_menu = (_game_mode == GM_MENU);

	if (path[0] == '\0') {
		MusLog("silent: song=%d (no WAV in that slot)", cs);
		return;   /* IsSongPlaying() holds when the install has no music at all */
	}

	_cur_stream = Adpcm_Open(path);
	if (_cur_stream == NULL) {
		MusLog("open FAILED: %s", path);
		return;
	}

	rate = Adpcm_Rate(_cur_stream);
	if (rate <= 0) rate = 22050;
	period = (int)(MUS_PAL_CLOCK / (unsigned long)rate);

	AmigaAudio_MusicSetVolume(VolToPaula(_cur_vol));
	if (!AmigaAudio_MusicStart(period, MUS_CHUNK, AmigaMusicRefill, _cur_stream)) {
		MusLog("MusicStart FAILED: %s", path);
		Adpcm_Close(_cur_stream);
		_cur_stream = NULL;
		return;
	}
	MusLog("play song %d -> %s (menu=%d)", cs, path, _cur_is_menu ? 1 : 0);
	(void)filename;   /* OpenTTD's synthetic .gm name is not opened */
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
	if (_cur_stream == NULL) {
		/* No WAV loaded. With NO music at all (no-music tier / empty dirs) claim
		 * "playing" so OpenTTD's loop stops calling PlaySong every tick and
		 * spamming the song number; otherwise report done so it advances. */
		return _song_count == 0 || _cat_path[0][0] == '\0';
	}
	/* Switch promptly when crossing the menu<->game boundary. */
	if (_cur_is_menu != (_game_mode == GM_MENU)) return false;
	return !AmigaAudio_MusicFinished();
}

void MusicDriver_Amiga::SetVolume(byte vol)
{
	_cur_vol = vol;
	AmigaAudio_MusicSetVolume(VolToPaula(vol));
}
