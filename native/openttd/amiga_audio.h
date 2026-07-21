/* Paula audio for OpenTTD on AmigaOS 68k - OS layer, plain C API.
 *
 * ALL Amiga OS calls live in amiga_audio.c: Amiga system headers cannot be
 * included from OpenTTD's C++ (name collisions break the build), so this
 * header exposes no Amiga types at all - only plain ints and void pointers.
 * Same split as amiga_gfx.c / amiga_gfx.h for video.
 *
 * Model (proven standalone in native/paulatest.c):
 *  - audio.device, all four channels allocated at once (ADCMD_ALLOCATE,
 *    mask 15, ADIOF_NOWAIT: if anything else holds Paula we fail fast and
 *    the game runs silent).
 *  - One CMD_WRITE per sound effect, one Paula channel per effect, played by
 *    DMA with ioa_Cycles = 1; the CPU does nothing after starting it.
 *  - Requests are sent with BeginIO() ONLY - SendIO()/DoIO() clear io_Flags,
 *    wiping ADIOF_PERVOL, which silently leaves the channel at volume 0.
 *  - Completion is polled with CheckIO(), never Wait(): the game is built
 *    thread_none and must never block.
 */

#ifndef AMIGA_AUDIO_H
#define AMIGA_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Number of Paula channels. Channels 0 and 3 route to the LEFT jack,
 * 1 and 2 to the RIGHT jack. */
#define AMIGA_AUDIO_CHANNELS 4

/* Open audio.device and allocate all four channels.
 * Returns 1 on success, 0 on failure (caller must then stay silent;
 * every other call below is safe but a no-op / failure in that state). */
int AmigaAudio_Open(void);

/* Abort any playing sound, free the channels, close the device.
 * Safe to call at any time, including after a failed/absent Open. */
void AmigaAudio_Close(void);

/* Allocate/free a sample buffer Paula DMA can read: Chip RAM, word-aligned.
 * Returns NULL when Chip RAM is exhausted. FreeSample(NULL) is a no-op. */
void *AmigaAudio_AllocSample(unsigned long bytes);
void AmigaAudio_FreeSample(void *p);

/* 1 if channel ch (0..3) is idle. Reaps a completed request (CheckIO +
 * WaitIO) as a side effect; never blocks. 0 while still playing. */
int AmigaAudio_ChannelIdle(int ch);

/* Start playing on channel ch: 8-bit signed samples in Chip RAM (from
 * AllocSample), even byte count (max 131070 - the Paula length register),
 * Paula period (>= 124 on PAL, i.e. <= ~28.6 kHz), hardware volume 0..64.
 * Plays once (ioa_Cycles = 1), entirely by DMA. Returns 1 if started,
 * 0 if not open / channel busy / bad arguments. */
int AmigaAudio_Play(int ch, void *chipdata, unsigned long bytes,
                    int period, int volume);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_AUDIO_H */
