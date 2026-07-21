/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native AmigaOS sound driver for OpenTTD 1.0.5: per-effect Paula playback.
 *
 * This file implements BOTH the SoundDriver ("amiga") and the whole Mx*
 * mixer API from mixer.h, and REPLACES mixer.cpp in the build (mixer.cpp is
 * removed from the source list; nothing else in the Amiga build calls
 * MxMixSamples - only the sdl/win32/allegro/cocoa sound drivers did, and
 * none of them is compiled here).
 *
 * Design (decided with the user): no software mixing. sound.cpp's call
 * sequence per effect is
 *     MxAllocateChannel() -> MxSetChannelRawSrc() -> MxSetChannelVolume()
 *     -> MxActivateChannel()
 * and is strictly sequential (thread_none build, no reentrancy). We convert
 * the sample once into Chip RAM at MxSetChannelRawSrc, and at
 * MxActivateChannel hand it to a free Paula channel which plays it by DMA;
 * the CPU does nothing afterwards. If all four channels are busy, the sound
 * is dropped - explicitly accepted, no voice stealing (yet).
 *
 * AMBIENT CAP (user request): ambient/background effects - jungle birds,
 * wind, industry idle noises - may occupy AT MOST 2 of the 4 Paula channels,
 * so ambience can never starve gameplay effects (UI, construction,
 * vehicles). The mixer API does not carry the sound ID, so sound.cpp's
 * StartSound() got a two-line patch calling MxSetNextSoundID(sound_id)
 * right before MxAllocateChannel(); the ID->class table lives here.
 *
 * MEMORY OWNERSHIP (verified in mixer.cpp/sound.cpp of 1.0.5):
 * SetBankSource() MallocT's the sample buffer and passes it to
 * MxSetChannelRawSrc; from then on it belongs to the mixer (the original
 * frees it with free() lazily in MxAllocateChannel). Nothing touches the
 * buffer after MxSetChannelRawSrc returns, so we convert it into our Chip
 * RAM copy and free() it before returning. The Chip RAM copy is ours and is
 * freed lazily when the slot is next reused - by then playback is long over.
 *
 * VOLUME (verified in sound.cpp of 1.0.5): MxSetChannelVolume receives
 *     left  = volume * 8 * (16 - panning)
 *     right = volume * 8 * (16 + panning),   panning in [-16, +16]
 * with volume = (sound->volume * effect_vol_scaled) / 128, i.e. 0..~128.
 * So the LOUDER side is volume*8*(16+|panning|) = volume*128 .. volume*256,
 * full-volume centre = 16384, and a hard-panned sound has 0 on one side.
 * Paula's register is 0..64: we take the louder side (never the side we
 * happened to pick a channel on - a hard-panned sound would floor to 0),
 * scale with ROUNDING ((v + 128) >> 8), and clamp any non-zero request to
 * at least 1 so a quiet effect is quiet instead of silent.
 *
 * SAMPLE CONVERSION (cheap, no tables):
 *  - Paula tops out at ~28.6 kHz on PAL (period floor 124). Halving the
 *    rate = taking every 2nd sample, so 44.1 kHz sources become 22.05 kHz;
 *    11.025/22.05 kHz sources are played at their native rate. The rate is
 *    taken per sample from the WAV header, never assumed.
 *  - sound.cpp has already made 8-bit data signed and byte-swapped 16-bit
 *    data to native (big-endian) order, so 16->8 bit is just ">> 8".
 *
 * DIAGNOSTICS: capped, one-run-conclusive log in PROGDIR:amiga_snd.log
 * (the host can only read it after the game exits). Per-request lines for
 * the first AMIGA_SND_REQLOG requests (source rate/bits/length, requested
 * L/R volume, computed register, outcome, channel), a counters snapshot
 * every ~30 s from MainLoop() (which also proves MainLoop runs on the
 * title screen), and a final totals line from Stop().
 */

#include "../stdafx.h"
#include "../debug.h"
#include "../mixer.h"
#include "../sound_type.h"
#include "amiga_s.h"
#include "amiga_audio.h"

#include <stdio.h>
#include <stdarg.h>

/** Factory for the Amiga Paula sound driver. */
static FSoundDriver_Amiga iFSoundDriver_Amiga;

/* PAL Paula clock: sample rate = PAL_CLOCK / period. */
static const uint32 PAL_CLOCK = 3546895;
static const uint32 PAULA_MIN_PERIOD = 124;    /* PAL DMA floor, ~28.6 kHz */
static const uint32 PAULA_MAX_BYTES = 131070;  /* length register: 65535 words */

/* At most this many of the 4 channels may play ambient effects at once. */
static const int MAX_AMBIENT_CHANNELS = 2;

static bool _paula_active = false;

struct MixerChannel {
	int8 *chip;        ///< converted 8-bit sample in Chip RAM, owned here
	uint32 chip_len;   ///< even byte count handed to Paula
	uint32 period;     ///< Paula period register value
	uint vol_left;     ///< as passed by sound.cpp, 0..~32768
	uint vol_right;
	int hw;            ///< Paula channel while playing, -1 otherwise
	int snd_id;        ///< SoundID from the StartSound hook, -1 if unknown
	bool ambient;      ///< counts against MAX_AMBIENT_CHANNELS
	/* diagnostics, latched at MxSetChannelRawSrc */
	uint dbg_rate;
	uint dbg_bits;
	uint32 dbg_insamples;
};

static MixerChannel _mx[AMIGA_AUDIO_CHANNELS];

/* Set by the 2-line hook in sound.cpp just before MxAllocateChannel(). */
static int _next_sound_id = -1;

/* ---- diagnostics ------------------------------------------------------- */

#define AMIGA_SND_LOG      "PROGDIR:amiga_snd.log"
#define AMIGA_SND_REQLOG   24   /* per-request lines, then counters only */
#define AMIGA_SND_SNAPLOG  40   /* periodic snapshot lines */

static uint _cnt_req = 0;       /* MxAllocateChannel calls = requests seen */
static uint _cnt_noslot = 0;    /* MxAllocateChannel returned NULL */
static uint _cnt_convfail = 0;  /* RawSrc rejected (rate 0/too short/no Chip) */
static uint _cnt_play = 0;      /* handed to Paula */
static uint _cnt_nochan = 0;    /* activate: all four channels busy */
static uint _cnt_ambfull = 0;   /* activate: ambient cap hit */
static uint _cnt_reg0 = 0;      /* activate: volume register was 0 */
static uint _cnt_reap = 0;      /* completed writes collected */
static uint _cnt_ml = 0;        /* MainLoop ticks (proves it runs in menu) */
static uint _log_req_lines = 0;
static uint _log_snap_lines = 0;

static void SndLog(const char *fmt, ...)
{
	char buf[192];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);

	FILE *f = fopen(AMIGA_SND_LOG, "a");
	if (f == NULL) return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}

/** True for background/ambience effects (tile-loop generated): jungle
 * birds, wind, industry idle noises. Everything else - UI, construction,
 * vehicles, disasters - is a gameplay effect and is never capped. */
static bool IsAmbientSound(int id)
{
	switch (id) {
		/* nature, from tree_cmd.cpp tile loop */
		case SND_34_WIND:
		case SND_39_HEAVY_WIND:
		case SND_42_LOON_BIRD:
		case SND_43_LION:
		case SND_44_MONKEYS:
		case SND_48_DISTANT_BIRD:
		/* industry idle animations, from industry_cmd.cpp */
		case SND_0B_MINING_MACHINERY:
		case SND_0C_ELECTRIC_SPARK:
		case SND_29_RIP:
		case SND_2A_EXTRACT_AND_POP:
		case SND_2B_COMEDY_HIT:
		case SND_2C_MACHINERY:
		case SND_2D_RIP_2:
		case SND_2E_EXTRACT_AND_POP:
		case SND_30_CARTOON_SOUND:
		case SND_36_CARTOON_CRASH:
		case SND_37_BALLOON_SQUEAK:
		case SND_38_CHAINSAW:
			return true;
		default:
			return false;
	}
}

/** Hook target for the 2-line patch in sound.cpp's StartSound(). */
void MxSetNextSoundID(int id)
{
	_next_sound_id = id;
}

static int ChannelsInUse()
{
	int n = 0;
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		if (_mx[i].hw >= 0) n++;
	}
	return n;
}

/** Unbind every slot whose Paula write has completed (non-blocking). */
static void ReapChannels()
{
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		if (_mx[i].hw >= 0 && AmigaAudio_ChannelIdle(_mx[i].hw)) {
			_mx[i].hw = -1;
			_cnt_reap++;
		}
	}
}

MixerChannel *MxAllocateChannel()
{
	int snd_id = _next_sound_id;
	_next_sound_id = -1;

	if (!_paula_active) return NULL;
	_cnt_req++;

	ReapChannels();
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		MixerChannel *mc = &_mx[i];
		if (mc->hw >= 0) continue;   /* still playing */
		/* Lazy free of the previous effect's Chip RAM copy, mirroring the
		 * original mixer's lazy free() of mc->memory in this same spot. */
		AmigaAudio_FreeSample(mc->chip);
		mc->chip = NULL;
		mc->chip_len = 0;
		mc->period = 0;
		mc->vol_left = mc->vol_right = 0;
		mc->snd_id = snd_id;
		mc->ambient = IsAmbientSound(snd_id);
		mc->dbg_rate = 0;
		mc->dbg_bits = 0;
		mc->dbg_insamples = 0;
		return mc;
	}
	_cnt_noslot++;
	if (_log_req_lines < AMIGA_SND_REQLOG) {
		_log_req_lines++;
		SndLog("R%03u id=%d NOSLOT use=%d", _cnt_req, snd_id, ChannelsInUse());
	}
	return NULL;   /* all four channels busy: sound.cpp just drops the sound */
}

void MxSetChannelRawSrc(MixerChannel *mc, int8 *mem, size_t size, uint rate, bool is16bit)
{
	/* We own mem from here on (see MEMORY OWNERSHIP above): every path
	 * below must free() it exactly once. */
	if (mc == NULL || !_paula_active || mem == NULL || rate == 0) {
		_cnt_convfail++;
		free(mem);
		return;
	}

	uint32 n = (uint32)(is16bit ? size / 2 : size);   /* sample count */
	mc->dbg_rate = rate;
	mc->dbg_bits = is16bit ? 16 : 8;
	mc->dbg_insamples = n;

	/* Halve the rate (= take every 2^k-th sample) until Paula can do it.
	 * The rate comes from this sample's WAV header - never assumed. */
	uint32 step = 1;
	uint32 r = rate;
	while (PAL_CLOCK / r < PAULA_MIN_PERIOD && step < 8) {
		r >>= 1;
		step <<= 1;
	}

	uint32 out = n / step;
	out &= ~(uint32)1;                       /* Paula needs an even length */
	if (out > PAULA_MAX_BYTES) out = PAULA_MAX_BYTES;
	if (out < 2) {
		_cnt_convfail++;
		if (_log_req_lines < AMIGA_SND_REQLOG) {
			_log_req_lines++;
			SndLog("R%03u id=%d r=%u b=%u in=%u CONVFAIL(short)",
					_cnt_req, mc->snd_id, rate, mc->dbg_bits, n);
		}
		free(mem);
		return;   /* mc->chip stays NULL -> MxActivateChannel is a no-op */
	}

	mc->chip = (int8 *)AmigaAudio_AllocSample(out);
	if (mc->chip == NULL) {
		_cnt_convfail++;
		if (_log_req_lines < AMIGA_SND_REQLOG) {
			_log_req_lines++;
			SndLog("R%03u id=%d r=%u b=%u in=%u CONVFAIL(chip %u)",
					_cnt_req, mc->snd_id, rate, mc->dbg_bits, n, out);
		}
		free(mem);
		return;   /* Chip RAM exhausted: drop this sound, keep running */
	}

	if (is16bit) {
		/* Already native-endian signed 16-bit (sound.cpp byte-swapped). */
		const int16 *src = (const int16 *)mem;
		for (uint32 i = 0; i < out; i++) mc->chip[i] = (int8)(src[i * step] >> 8);
	} else {
		/* Already signed 8-bit (sound.cpp converted from unsigned). */
		for (uint32 i = 0; i < out; i++) mc->chip[i] = mem[i * step];
	}
	mc->chip_len = out;
	mc->period = PAL_CLOCK / r;

	free(mem);
}

void MxSetChannelVolume(MixerChannel *mc, uint left, uint right)
{
	if (mc == NULL) return;
	mc->vol_left = left;
	mc->vol_right = right;
}

void MxActivateChannel(MixerChannel *mc)
{
	/* Paula stereo routing: 0+3 -> LEFT jack, 1+2 -> RIGHT jack. */
	static const int left_first[AMIGA_AUDIO_CHANNELS]  = {0, 3, 1, 2};
	static const int right_first[AMIGA_AUDIO_CHANNELS] = {1, 2, 0, 3};

	if (mc == NULL || !_paula_active || mc->chip == NULL || mc->hw >= 0) return;

	/* Louder side, scaled 0..~32768 -> 0..64 with rounding; any non-zero
	 * request plays at >= 1 so quiet effects are quiet, not silent. */
	uint v = (mc->vol_left > mc->vol_right) ? mc->vol_left : mc->vol_right;
	int reg = (int)((v + 128) >> 8);
	if (reg == 0 && v > 0) reg = 1;
	if (reg > 64) reg = 64;

	/* duration in ms, for the log: len * period / (PAL_CLOCK/1000) */
	uint32 ms = (uint32)((unsigned long long)mc->chip_len * mc->period * 1000 / PAL_CLOCK);

	if (reg == 0) {
		_cnt_reg0++;
		if (_log_req_lines < AMIGA_SND_REQLOG) {
			_log_req_lines++;
			SndLog("R%03u id=%d amb=%d r=%u b=%u in=%u out=%u per=%u ms=%u L=%u R=%u REG0",
					_cnt_req, mc->snd_id, mc->ambient ? 1 : 0, mc->dbg_rate, mc->dbg_bits,
					mc->dbg_insamples, mc->chip_len, mc->period, ms,
					mc->vol_left, mc->vol_right);
		}
		return;
	}

	ReapChannels();

	/* Ambient cap: at most MAX_AMBIENT_CHANNELS of the 4 may be ambience. */
	if (mc->ambient) {
		int amb = 0;
		for (int j = 0; j < AMIGA_AUDIO_CHANNELS; j++) {
			if (_mx[j].hw >= 0 && _mx[j].ambient) amb++;
		}
		if (amb >= MAX_AMBIENT_CHANNELS) {
			_cnt_ambfull++;
			if (_log_req_lines < AMIGA_SND_REQLOG) {
				_log_req_lines++;
				SndLog("R%03u id=%d amb=1 r=%u b=%u out=%u ms=%u L=%u R=%u reg=%d AMBFULL",
						_cnt_req, mc->snd_id, mc->dbg_rate, mc->dbg_bits,
						mc->chip_len, ms, mc->vol_left, mc->vol_right, reg);
			}
			return;
		}
	}

	/* Prefer a channel on the dominant side; take any free one otherwise.
	 * If a start fails unexpectedly, try the remaining candidates. */
	const int *order = (mc->vol_left >= mc->vol_right) ? left_first : right_first;

	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		int ch = order[i];
		bool taken = false;
		for (int j = 0; j < AMIGA_AUDIO_CHANNELS; j++) {
			if (_mx[j].hw == ch) taken = true;
		}
		if (taken || !AmigaAudio_ChannelIdle(ch)) continue;
		if (AmigaAudio_Play(ch, mc->chip, mc->chip_len, (int)mc->period, reg)) {
			mc->hw = ch;
			_cnt_play++;
			if (_log_req_lines < AMIGA_SND_REQLOG) {
				_log_req_lines++;
				SndLog("R%03u id=%d amb=%d r=%u b=%u in=%u out=%u per=%u ms=%u L=%u R=%u reg=%d ch%d use=%d",
						_cnt_req, mc->snd_id, mc->ambient ? 1 : 0, mc->dbg_rate,
						mc->dbg_bits, mc->dbg_insamples, mc->chip_len, mc->period,
						ms, mc->vol_left, mc->vol_right, reg, ch, ChannelsInUse());
			}
			return;
		}
	}

	_cnt_nochan++;
	if (_log_req_lines < AMIGA_SND_REQLOG) {
		_log_req_lines++;
		SndLog("R%03u id=%d amb=%d ms=%u L=%u R=%u reg=%d NOCHAN use=%d",
				_cnt_req, mc->snd_id, mc->ambient ? 1 : 0, ms,
				mc->vol_left, mc->vol_right, reg, ChannelsInUse());
	}
	/* All four channels busy: the sound is dropped by design (no stealing). */
}

bool MxInitialize(uint rate)
{
	/* Nothing to do: there is no software mixing and no output rate - each
	 * effect plays at its own Paula period. Kept for the mixer.h contract. */
	return true;
}

void MxMixSamples(void *buffer, uint samples)
{
	/* Never called on the Amiga path (no compiled driver pulls samples).
	 * If something ever does, hand back silence rather than garbage. */
	memset(buffer, 0, sizeof(int16) * 2 * samples);
}

const char *SoundDriver_Amiga::Start(const char * const *parm)
{
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		_mx[i].chip = NULL;
		_mx[i].chip_len = 0;
		_mx[i].hw = -1;
		_mx[i].snd_id = -1;
		_mx[i].ambient = false;
	}
	_paula_active = (AmigaAudio_Open() != 0);

	remove(AMIGA_SND_LOG);   /* fresh log per run */
	SndLog("AMIGA-PAULA-SOUND-v2 %s",
			_paula_active ? "Paula: 4 channels allocated" : "Paula UNAVAILABLE - running silent");

	DEBUG(driver, 1, "amiga_s: AMIGA-PAULA-SOUND-v2 %s",
			_paula_active ? "(4 Paula channels allocated)" : "(audio.device unavailable, running silent)");
	/* NEVER report failure: in 1.0.5 an explicitly selected driver whose
	 * Start returns an error is a fatal usererror(). Degrade to silence. */
	return NULL;
}

/* Called once per game tick from GameLoop() - including on the title
 * screen. Collects finished Paula writes so channels return to the pool
 * even when no new sound is requested, and emits a capped snapshot line
 * (~every 30 s) so one run shows whether requests arrive in menu mode. */
void SoundDriver_Amiga::MainLoop()
{
	if (!_paula_active) return;
	ReapChannels();
	_cnt_ml++;
	if ((_cnt_ml & 1023) == 0 && _log_snap_lines < AMIGA_SND_SNAPLOG) {
		_log_snap_lines++;
		SndLog("S ml=%u req=%u play=%u noslot=%u nochan=%u ambfull=%u reg0=%u conv=%u reap=%u use=%d",
				_cnt_ml, _cnt_req, _cnt_play, _cnt_noslot, _cnt_nochan,
				_cnt_ambfull, _cnt_reg0, _cnt_convfail, _cnt_reap, ChannelsInUse());
	}
}

void SoundDriver_Amiga::Stop()
{
	SndLog("F req=%u play=%u noslot=%u nochan=%u ambfull=%u reg0=%u conv=%u reap=%u ml=%u use=%d",
			_cnt_req, _cnt_play, _cnt_noslot, _cnt_nochan, _cnt_ambfull,
			_cnt_reg0, _cnt_convfail, _cnt_reap, _cnt_ml, ChannelsInUse());

	_paula_active = false;
	AmigaAudio_Close();   /* aborts anything still playing, frees channels */
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		AmigaAudio_FreeSample(_mx[i].chip);
		_mx[i].chip = NULL;
		_mx[i].hw = -1;
	}
}
