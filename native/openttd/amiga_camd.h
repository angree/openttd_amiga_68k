/* MIDI playback for the native AmigaOS port of OpenTTD 1.0.5.
 *
 * The events go out through camd.library, which is the AmigaOS standard MIDI
 * router: it owns the hardware (serial MIDI interface, mt32-pi, a soundcard
 * driver, ...) and every application just hands it note/controller messages.
 * We therefore do NOT synthesize anything - we read a Standard MIDI File and
 * play it into CAMD's "out.0" cluster, which is where the MidiPorts prefs
 * program routes the user's interface by default.
 *
 * Keep this header free of both AmigaOS and OpenTTD types: it is included from
 * amiga_m.cpp, which must never see the proto headers (the OTTD_Point collision
 * amiga_m.cpp documents).
 */
#ifndef AMIGA_CAMD_H
#define AMIGA_CAMD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Is camd.library there at all? Opens and closes it, nothing more, so it is
 * cheap enough to call from a settings callback. Returns 1 if MIDI has any
 * chance of working on this machine. */
int AmigaMidi_Probe(void);

/* Open camd.library and start the player process. Returns 1 on success, 0 if
 * MIDI is not available on this machine (no camd.library, no free signals,
 * nothing listening on "out.0"). The caller falls back to sampled music. */
int AmigaMidi_Start(void);

/* Stop everything and give camd.library back. Safe if Start() failed. */
void AmigaMidi_Shutdown(void);

/* Play one Standard MIDI File (a .mid or an original .gm - same format).
 * Returns 1 if the file was accepted, 0 if it could not be read or parsed. */
int AmigaMidi_Play(const char *path);

/* Silence the output and forget the current song. */
void AmigaMidi_Stop(void);

/* 1 while a song is still running, 0 once it has played to its end. */
int AmigaMidi_IsPlaying(void);

/* OpenTTD's 0..127 music volume. Applied as MIDI channel volume (CC 7), so
 * it scales whatever the file itself asked for instead of replacing it. */
void AmigaMidi_SetVolume(int vol);

/* Last failure, as a short English sentence for the log. Never NULL. */
const char *AmigaMidi_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_CAMD_H */
