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
#include "../settings_type.h"           /* _settings_client.amiga.music_source */
#include "amiga_m.h"

extern "C" {
#include "../sound/amiga_audio.h"
#include "../sound/amiga_adpcm.h"
#include "amiga_mscan.h"
#include "amiga_camd.h"
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

/* Which of the three sources the catalogue was built from. Decided ONCE, at
 * the first scan, because music.cpp asks for the song list while the base sets
 * are being scanned - long before this driver is started - and OpenTTD keeps
 * that list for the rest of the run. Changing amiga.music_source therefore
 * needs a restart, which is what the setting says.
 * 0 = sampled WAV through Paula, 1 = MIDI out through camd.library. */
static int _midi_mode = 0;

/* Why MIDI was not used, when the player asked for it. Shown ONCE in a normal
 * error window (see MusicLoop) - a log file nobody knows about is not an answer
 * to "I picked MIDI and it played the samples". */
static char _midi_failure[128];
static bool _midi_failure_pending = false;

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

/* ------------------------------------------------------------------ MIDI --
 *
 * The MIDI sources live in PROGDIR:gm/ and describe themselves with a music
 * base set file (.obm) sitting next to the songs. We read it for two things a
 * bare directory listing cannot give us: the running order the set intends,
 * and the real track titles - "GM_TT04.GM" is a file name, not a song name.
 *
 * A set splits its songs into theme / old / new / ezy. This port ignores
 * OpenTTD's era selection (see the header comment), so the mapping is the one
 * the WAV directories already use: theme is track 1, new + ezy are the in-game
 * rotation, and old is the "Old Style" playlist.
 */

#define OBM_MAX_NAMES  80
#define OBM_MAX_NORMAL 24
#define OBM_MAX_OLD    16

struct ObmSet {
	char theme[AMIGA_MUSIC_NAME_MAX];
	char normal[OBM_MAX_NORMAL][AMIGA_MUSIC_NAME_MAX];
	int  normal_n;
	char old[OBM_MAX_OLD][AMIGA_MUSIC_NAME_MAX];
	int  old_n;
	char name_file[OBM_MAX_NAMES][AMIGA_MUSIC_NAME_MAX];
	char name_text[OBM_MAX_NAMES][AMIGA_MUSIC_NAME_MAX];
	int  names_n;
	bool ok;
};

static ObmSet _obm;

static int MusLower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

static bool MusEqualNoCase(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (MusLower((unsigned char)*a) != MusLower((unsigned char)*b)) return false;
		a++; b++;
	}
	return *a == *b;
}

/* Copy the value part of "key = value", trimmed at both ends. */
static void MusTrimCopy(char *dst, size_t dst_size, const char *src)
{
	size_t len;
	while (*src == ' ' || *src == '\t') src++;
	snprintf(dst, dst_size, "%s", src);
	len = strlen(dst);
	while (len > 0 && (dst[len - 1] == ' '  || dst[len - 1] == '\t' ||
	                   dst[len - 1] == '\r' || dst[len - 1] == '\n')) {
		dst[--len] = '\0';
	}
}

/* Read one .obm. Returns false when it is missing or says nothing, in which
 * case the caller falls back to a plain directory listing. */
static bool ReadObm(const char *path)
{
	char line[512];
	char key[64];
	int  section = 0;    /* 1 = [files], 2 = [names] */
	FILE *f;

	memset(&_obm, 0, sizeof(_obm));

	f = fopen(path, "r");
	if (f == NULL) return false;

	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq;
		if (line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[') {
			section = 0;
			if (strncmp(line, "[files]", 7) == 0) section = 1;
			if (strncmp(line, "[names]", 7) == 0) section = 2;
			continue;
		}
		if (section == 0) continue;

		eq = strchr(line, '=');
		if (eq == NULL) continue;
		*eq = '\0';
		MusTrimCopy(key, sizeof(key), line);
		if (key[0] == '\0') continue;

		if (section == 1) {
			char val[AMIGA_MUSIC_NAME_MAX];
			MusTrimCopy(val, sizeof(val), eq + 1);
			if (val[0] == '\0') continue;      /* an unused slot, e.g. "old_8 =" */
			if (MusEqualNoCase(key, "theme")) {
				snprintf(_obm.theme, sizeof(_obm.theme), "%s", val);
			} else if ((strncmp(key, "new_", 4) == 0 || strncmp(key, "ezy_", 4) == 0) &&
					_obm.normal_n < OBM_MAX_NORMAL) {
				snprintf(_obm.normal[_obm.normal_n++], AMIGA_MUSIC_NAME_MAX, "%s", val);
			} else if (strncmp(key, "old_", 4) == 0 && _obm.old_n < OBM_MAX_OLD) {
				snprintf(_obm.old[_obm.old_n++], AMIGA_MUSIC_NAME_MAX, "%s", val);
			}
		} else if (_obm.names_n < OBM_MAX_NAMES) {
			char val[AMIGA_MUSIC_NAME_MAX];
			MusTrimCopy(val, sizeof(val), eq + 1);
			if (val[0] == '\0') continue;
			snprintf(_obm.name_file[_obm.names_n], AMIGA_MUSIC_NAME_MAX, "%s", key);
			snprintf(_obm.name_text[_obm.names_n], AMIGA_MUSIC_NAME_MAX, "%s", val);
			_obm.names_n++;
		}
	}
	fclose(f);

	_obm.ok = (_obm.theme[0] != '\0' || _obm.normal_n > 0 || _obm.old_n > 0);
	return _obm.ok;
}

/* The set's own title for a file, or the bare file name when it has none. */
static const char *ObmTitle(const char *file, const char *fallback)
{
	int i;
	for (i = 0; i < _obm.names_n; i++) {
		if (MusEqualNoCase(_obm.name_file[i], file)) return _obm.name_text[i];
	}
	return fallback;
}

/* Which scanned entry is this .obm file name? Compared without the suffix,
 * because that is what the scanner hands back, and without regard to case,
 * because orig_win.obm spells the files "GM_TT00.GM" while a real drawer may
 * hold "gm_tt00.gm". Returns -1 when the set names a song that is not there. */
static int FindScanned(int n, const char *file, const char *ext)
{
	char base[AMIGA_MUSIC_NAME_MAX];
	size_t len, ext_len;
	int i;

	snprintf(base, sizeof(base), "%s", file);
	len = strlen(base);
	ext_len = strlen(ext);
	if (len > ext_len && MusEqualNoCase(base + len - ext_len, ext)) base[len - ext_len] = '\0';

	for (i = 0; i < n; i++) {
		if (MusEqualNoCase(_dir_names[i], base)) return i;
	}
	return -1;
}

/* Build the catalogue out of PROGDIR:gm/. Returns the number of songs found,
 * or 0 when there is nothing to play - the caller then falls back to WAV. */
static int ScanMidiDirectory(int source)
{
	const char *ext = (source == 2) ? ".gm" : ".mid";
	const char *obm = (source == 2) ? "PROGDIR:gm/orig_win.obm" : "PROGDIR:gm/openmsx.obm";
	bool used[MUS_MAX_DIR_TRACKS];
	int n, i, j, idx;

	n = AmigaMusic_ScanDirExt("PROGDIR:gm", ext, _dir_paths, _dir_names, MUS_MAX_DIR_TRACKS);
	MusLog("gm scan: %d file(s) matching *%s", n, ext);
	if (n == 0) return 0;

	for (i = 0; i < MUS_MAX_DIR_TRACKS; i++) used[i] = false;

	if (ReadObm(obm)) {
		MusLog("%s: theme=%d normal=%d old=%d names=%d", obm,
				_obm.theme[0] != '\0' ? 1 : 0, _obm.normal_n, _obm.old_n, _obm.names_n);

		idx = (_obm.theme[0] != '\0') ? FindScanned(n, _obm.theme, ext) : -1;
		if (idx >= 0) {
			CatAdd(_dir_paths[idx], ObmTitle(_obm.theme, _dir_names[idx]));
			used[idx] = true;
		} else {
			CatAdd("", "Amiga music");   /* song 1 stays reserved for the theme */
		}

		for (i = 0; i < _obm.normal_n; i++) {
			idx = FindScanned(n, _obm.normal[i], ext);
			if (idx < 0 || used[idx]) continue;
			if (CatAdd(_dir_paths[idx], ObmTitle(_obm.normal[i], _dir_names[idx]))) {
				_normal_count++;
				used[idx] = true;
			}
		}

		/* Anything in the drawer the set does not mention still plays: someone
		 * who drops their own .mid in should hear it, not have it ignored.
		 * Songs the set files under "old" are left for the loop below. */
		for (i = 0; i < n; i++) {
			bool in_old = false;
			if (used[i]) continue;
			for (j = 0; j < _obm.old_n; j++) {
				if (FindScanned(n, _obm.old[j], ext) == i) { in_old = true; break; }
			}
			if (in_old) continue;
			if (CatAdd(_dir_paths[i], _dir_names[i])) {
				_normal_count++;
				used[i] = true;
			}
		}

		for (i = 0; i < _obm.old_n; i++) {
			idx = FindScanned(n, _obm.old[i], ext);
			if (idx < 0 || used[idx]) continue;
			if (CatAdd(_dir_paths[idx], ObmTitle(_obm.old[i], _dir_names[idx]))) {
				_old_count++;
				used[idx] = true;
			}
		}
	} else {
		/* No description file: play what is there, in the order AmigaDOS gave
		 * it, with the first track doubling as the menu theme. */
		MusLog("no %s - plain directory listing", obm);
		CatAdd(_dir_paths[0], _dir_names[0]);
		for (i = 0; i < n; i++) {
			if (CatAdd(_dir_paths[i], _dir_names[i])) _normal_count++;
		}
	}

	return _song_count;
}

/* ------------------------------------------- reading the config TOO EARLY --
 *
 * openttd.cfg is NOT loaded yet when this scanner first runs. openttd.cpp does
 *
 *     BaseGraphics::FindSets();
 *     BaseSounds::FindSets();
 *     BaseMusic::FindSets();     <- music.cpp asks us for the song list here
 *     ...
 *     LoadFromConfig();          <- and the settings only arrive here
 *
 * so _settings_client.amiga.music_source is still zero at that point, whatever
 * the player put in the file. That is exactly the bug the user hit: they picked
 * MIDI, and got the sampled music with no complaint, because by the time
 * anything could complain the catalogue had already been built from music/.
 *
 * It cost an afternoon to find because it hid: MusicDriver_Amiga::Start() runs
 * AFTER the config is loaded, deletes the log and scans again, so the log
 * showed the right answer while the song list OpenTTD kept was built from the
 * wrong one.
 *
 * Moving LoadFromConfig() up would be a change to the startup order of a
 * program that has plenty of other opinions about it. Reading the two keys we
 * need, ourselves, out of the file the game is about to read anyway, is a much
 * smaller thing to be wrong about - and it cannot disagree with the settings
 * system, because both read the same file. */
static int AmigaCfgEnum(const char *key, const char *const *names, int nnames, int def)
{
	char line[512];
	char want[64];
	bool in_amiga = false;
	FILE *f;
	int result = def;

	snprintf(want, sizeof(want), "%s", key);

	f = fopen("PROGDIR:openttd.cfg", "r");
	if (f == NULL) return def;

	while (fgets(line, sizeof(line), f) != NULL) {
		char k[64], v[64];
		char *eq;

		if (line[0] == '[') {
			in_amiga = (strncmp(line, "[amiga]", 7) == 0);
			continue;
		}
		if (!in_amiga) continue;

		eq = strchr(line, '=');
		if (eq == NULL) continue;
		*eq = '\0';
		MusTrimCopy(k, sizeof(k), line);
		if (!MusEqualNoCase(k, want)) continue;
		MusTrimCopy(v, sizeof(v), eq + 1);

		if (v[0] >= '0' && v[0] <= '9') {
			result = v[0] - '0';
		} else {
			int i;
			for (i = 0; i < nnames; i++) {
				if (MusEqualNoCase(v, names[i])) { result = i; break; }
			}
		}
		break;
	}
	fclose(f);

	if (result < 0 || result >= nnames) result = def;
	return result;
}

static void ScanMusicDirectories()
{
	int n, i;
	int source, routing;

	_song_count = 0;
	_normal_count = 0;
	_old_count = 0;
	_midi_mode = 0;

	/* Sources 1 and 2 play MIDI through camd.library instead of sampled WAV.
	 * Both can fail on a machine that has no MIDI set up at all, and a player
	 * left with silence would have no idea why, so every failure falls back to
	 * the sampled music and says what happened in the log. */
	{
		static const char *const src_names[] = { "sampled", "openmsx", "original" };
		static const char *const out_names[] = { "auto", "camd", "serial" };
		source = AmigaCfgEnum("music_source", src_names, 3, 0);
		routing = AmigaCfgEnum("midi_out",    out_names, 3, 0);
	}
	MusLog("config: music_source=%d midi_out=%d", source, routing);
	if (source != 0) {
		AmigaMidi_SetRouting(routing);
		if (AmigaMidi_Start()) {
			if (ScanMidiDirectory(source) > 0) {
				_midi_mode = 1;
				_scan_done = true;
				MusLog("MIDI catalogue: songs=%d (normal=%d old=%d)",
						_song_count, _normal_count, _old_count);
				return;
			}
			MusLog("nothing playable in PROGDIR:gm - using sampled music");
			snprintf(_midi_failure, sizeof(_midi_failure), (source == 2)
					? "no GM_TT*.GM files in the gm drawer"
					: "no .mid files in the gm drawer");
			_midi_failure_pending = true;
			AmigaMidi_Shutdown();
		} else {
			MusLog("MIDI unavailable: %s - using sampled music", AmigaMidi_LastError());
			snprintf(_midi_failure, sizeof(_midi_failure), "%s", AmigaMidi_LastError());
			_midi_failure_pending = true;
		}
		/* Whatever a half-finished MIDI scan added must not be kept. */
		_song_count = 0;
		_normal_count = 0;
		_old_count = 0;
	}

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

/* Consumed by music_gui.cpp's MusicLoop, which shows it once and clears it.
 * Returns NULL when there is nothing to report. */
extern "C" const char *AmigaMusic_MidiFailure(void)
{
	return _midi_failure_pending ? _midi_failure : NULL;
}
extern "C" void AmigaMusic_MidiFailureShown(void) { _midi_failure_pending = false; }

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
	/* Which MIDI route was taken is logged by the player itself, in
	 * amiga_midi.log - naming camd.library here was simply wrong once the
	 * serial route existed. */
	MusLog(_midi_mode
			? "AMIGA-MUSIC-v4 started: MIDI (see amiga_midi.log for the route)"
			: "AMIGA-MUSIC-v4 started: sampled music through Paula");
	return NULL;   /* never fail: an explicit driver failure is fatal upstream */
}

void MusicDriver_Amiga::Stop()
{
	this->StopSong();
	if (_midi_mode) AmigaMidi_Shutdown();
}

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
		MusLog("silent: song=%d (nothing in that slot)", cs);
		return;   /* IsSongPlaying() holds when the install has no music at all */
	}

	if (_midi_mode) {
		if (AmigaMidi_Play(path)) {
			MusLog("play song %d -> %s (menu=%d)", cs, path, _cur_is_menu ? 1 : 0);
		} else {
			MusLog("midi play FAILED: %s", path);
		}
		return;
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
	if (_midi_mode) {
		AmigaMidi_Stop();
		return;
	}

	AmigaAudio_MusicStop();
	if (_cur_stream != NULL) {
		Adpcm_Close(_cur_stream);
		_cur_stream = NULL;
	}
}

bool MusicDriver_Amiga::IsSongPlaying()
{
	if (_midi_mode) {
		/* Same shape as the sampled path below: an install with no music at all
		 * claims to be playing, so OpenTTD stops asking every tick. */
		if (_song_count == 0 || _cat_path[0][0] == '\0') return true;
		if (_cur_is_menu != (_game_mode == GM_MENU)) return false;
		return AmigaMidi_IsPlaying() != 0;
	}

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
	if (_midi_mode) {
		AmigaMidi_SetVolume((int)vol);   /* CC 7 on every channel */
		return;
	}
	AmigaAudio_MusicSetVolume(VolToPaula(vol));
}
