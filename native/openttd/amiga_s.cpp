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
 * the CPU does nothing afterwards. If all four channels are busy, the fifth
 * sound is simply dropped - explicitly accepted, no voice stealing.
 *
 * MEMORY OWNERSHIP (verified in mixer.cpp/sound.cpp of 1.0.5):
 * SetBankSource() MallocT's the sample buffer and passes it to
 * MxSetChannelRawSrc; from then on it belongs to the mixer (the original
 * frees it with free() lazily in MxAllocateChannel). Nothing touches the
 * buffer after MxSetChannelRawSrc returns, so we convert it into our Chip
 * RAM copy and free() it before returning. The Chip RAM copy is ours and is
 * freed lazily when the slot is next reused - by then playback is long over.
 *
 * SAMPLE CONVERSION (cheap, no tables):
 *  - OpenSFX is 44.1 kHz 16-bit mono; Paula tops out at ~28.6 kHz on PAL
 *    (period floor 124). 44.1 -> 22.05 kHz is exactly 2:1 - take every
 *    other sample. Generally: halve the rate until the period fits.
 *  - sound.cpp has already made 8-bit data signed and byte-swapped 16-bit
 *    data to native (big-endian) order, so 16->8 bit is just ">> 8".
 *  - Volume is a Paula register (0..64), no CPU-side scaling. sound.cpp
 *    sends per-side volumes up to ~16384 at full effect volume, centred
 *    (up to 2x that when panned hard); ">> 8" maps that onto 0..64.
 *    Proper stereo panning is limited to picking a Paula channel on the
 *    louder side: channels 0+3 are the LEFT jack, 1+2 the RIGHT jack.
 */

#include "../stdafx.h"
#include "../debug.h"
#include "../mixer.h"
#include "amiga_s.h"
#include "amiga_audio.h"

/** Factory for the Amiga Paula sound driver. */
static FSoundDriver_Amiga iFSoundDriver_Amiga;

/* PAL Paula clock: sample rate = PAL_CLOCK / period. */
static const uint32 PAL_CLOCK = 3546895;
static const uint32 PAULA_MIN_PERIOD = 124;    /* PAL DMA floor, ~28.6 kHz */
static const uint32 PAULA_MAX_BYTES = 131070;  /* length register: 65535 words */

/* True only between a successful AmigaAudio_Open() in Start() and Stop().
 * While false every Mx* entry point is an inert no-op (that also covers a
 * user forcing -s null: this file is still linked, but stays silent). */
static bool _paula_active = false;

/* One logical mixer slot per Paula channel. The PHYSICAL channel is chosen
 * only at MxActivateChannel (when the pan is finally known); hw ties the
 * slot to it while the sound plays. */
struct MixerChannel {
	int8 *chip;        ///< converted 8-bit sample in Chip RAM, owned here
	uint32 chip_len;   ///< even byte count handed to Paula
	uint32 period;     ///< Paula period register value
	uint vol_left;     ///< as passed by sound.cpp, ~0..32768
	uint vol_right;
	int hw;            ///< Paula channel while playing, -1 otherwise
};

static MixerChannel _mx[AMIGA_AUDIO_CHANNELS];

/** Unbind every slot whose Paula write has completed (non-blocking). */
static void ReapChannels()
{
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		if (_mx[i].hw >= 0 && AmigaAudio_ChannelIdle(_mx[i].hw)) _mx[i].hw = -1;
	}
}

MixerChannel *MxAllocateChannel()
{
	if (!_paula_active) return NULL;

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
		return mc;
	}
	return NULL;   /* all four channels busy: sound.cpp just drops the sound */
}

void MxSetChannelRawSrc(MixerChannel *mc, int8 *mem, size_t size, uint rate, bool is16bit)
{
	/* We own mem from here on (see MEMORY OWNERSHIP above): every path
	 * below must free() it exactly once. */
	if (mc == NULL || !_paula_active || mem == NULL || rate == 0) {
		free(mem);
		return;
	}

	uint32 n = (uint32)(is16bit ? size / 2 : size);   /* sample count */

	/* Halve the rate (= take every 2^k-th sample) until Paula can do it. */
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
		free(mem);
		return;   /* mc->chip stays NULL -> MxActivateChannel is a no-op */
	}

	mc->chip = (int8 *)AmigaAudio_AllocSample(out);
	if (mc->chip == NULL) {
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

	/* Hardware volume: the louder side, scaled ~16384 -> 64 and clamped
	 * (hard-panned sounds arrive up to ~32768). */
	uint v = (mc->vol_left > mc->vol_right) ? mc->vol_left : mc->vol_right;
	int vol = (int)(v >> 8);
	if (vol > 64) vol = 64;
	if (vol == 0) return;   /* inaudible anyway; Chip copy is freed lazily */

	/* Prefer a channel on the dominant side; take any free one otherwise. */
	const int *order = (mc->vol_left >= mc->vol_right) ? left_first : right_first;

	ReapChannels();
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		int ch = order[i];
		bool taken = false;
		for (int j = 0; j < AMIGA_AUDIO_CHANNELS; j++) {
			if (_mx[j].hw == ch) taken = true;
		}
		if (taken || !AmigaAudio_ChannelIdle(ch)) continue;
		if (AmigaAudio_Play(ch, mc->chip, mc->chip_len, (int)mc->period, vol)) {
			mc->hw = ch;
		}
		return;
	}
	/* All four channels busy: the fifth sound is dropped by design. */
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
	}
	_paula_active = (AmigaAudio_Open() != 0);
	DEBUG(driver, 1, "amiga_s: AMIGA-PAULA-SOUND-v1 %s",
			_paula_active ? "(4 Paula channels allocated)" : "(audio.device unavailable, running silent)");
	/* NEVER report failure: in 1.0.5 an explicitly selected driver whose
	 * Start returns an error is a fatal usererror(). Degrade to silence. */
	return NULL;
}

void SoundDriver_Amiga::Stop()
{
	_paula_active = false;
	AmigaAudio_Close();   /* aborts anything still playing, frees channels */
	for (int i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
		AmigaAudio_FreeSample(_mx[i].chip);
		_mx[i].chip = NULL;
		_mx[i].hw = -1;
	}
}
