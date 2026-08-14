/* MIDI playback through camd.library for the AmigaOS port of OpenTTD 1.0.5.
 *
 * WHY THIS FILE EXISTS AT ALL
 * --------------------------
 * camd.library is not a music player. It is AmigaOS's MIDI router: it knows
 * how to get a three-byte MIDI message out of the machine and into whatever
 * the user has plugged in (a serial MIDI interface, an mt32-pi, a soundcard
 * driver), and applications talk to it instead of to the hardware. What it
 * does NOT do is read a song off disk or keep time. That is this file: a
 * Standard MIDI File reader plus a clock.
 *
 * WHY A SEPARATE PROCESS
 * ----------------------
 * MIDI needs its events delivered on time, and OpenTTD's music driver has no
 * per-frame callback we could hang a clock on - IsSongPlaying() is polled from
 * the game loop, which stalls for as long as a frame takes. So the player runs
 * as its own AmigaOS process waiting on timer.device, and the game only ever
 * sends it "play this", "stop", "volume". A late frame then costs a dropped
 * frame, not a dragged beat.
 *
 * The process uses AmigaDOS I/O and AllocVec only - never stdio and never
 * malloc. libnix's stdio and its allocator belong to the process that started
 * the C library up, and reaching into them from a second process is how you
 * get a crash that only ever happens on someone else's machine.
 *
 * ROUTING
 * -------
 * We send to the CAMD cluster named "out.0", which is the conventional first
 * MIDI output and what the MidiPorts preferences program fills in by default.
 * If nothing is routed there the messages simply go nowhere - that is the
 * user's setup to fix, and we say so in the log rather than failing.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <midi/camd.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/camd.h>

#include <string.h>

#include "amiga_camd.h"

struct Library *CamdBase = NULL;

/* ------------------------------------------------------------------ limits */

#define MIDI_MAX_TRACKS   48
#define MIDI_MAX_FILE     (512UL * 1024)   /* no Standard MIDI File we play is near this */
#define MIDI_PATH_MAX     256

/* Events closer together than this are emitted in one go instead of costing
 * another timer round trip. Three milliseconds is inaudible, and it keeps a
 * dense song from asking timer.device for a thousand requests a second, which
 * a 68020 cannot afford. */
#define MIDI_MERGE_US     3000

/* ------------------------------------------------------------ shared state */

/* Written by the game task, read by the player process. Every change is made
 * under Forbid() and followed by a Signal(), so the player always sees a whole
 * request and never half of one. */
static volatile ULONG  g_req_seq;         /* bumped for every new request */
static volatile ULONG  g_done_seq;        /* what the player has picked up */
static char            g_req_path[MIDI_PATH_MAX];
static volatile LONG   g_req_kind;        /* 0 = stop, 1 = play g_req_path */
static volatile LONG   g_volume = 127;    /* OpenTTD's 0..127 */
static volatile LONG   g_vol_seq;         /* bumped when the volume changes */
static volatile LONG   g_playing;         /* 1 while a song is running */
static volatile LONG   g_quit;

static struct Task    *g_player;
static struct Task    *g_starter;
static volatile LONG   g_started;         /* 1 = player is up, -1 = it failed */

static struct MidiNode *g_midi_node;
static struct MidiLink *g_midi_link;

static const char *g_error = "";

/* ----------------------------------------------------------------- logging */

#define MIDI_LOG "PROGDIR:amiga_midi.log"
static LONG g_log_lines;

static void MidiLog(const char *s)
{
	BPTR fh;
	if (g_log_lines >= 80) return;
	g_log_lines++;
	fh = Open((STRPTR)MIDI_LOG, MODE_READWRITE);
	if (fh == 0) return;
	Seek(fh, 0, OFFSET_END);
	Write(fh, (APTR)s, (LONG)strlen(s));
	Write(fh, (APTR)"\n", 1);
	Close(fh);
}

/* ------------------------------------------------------- the file in memory */

typedef struct {
	const UBYTE *p;        /* read cursor */
	const UBYTE *end;
	ULONG        next_tick;
	UBYTE        running;  /* running status byte */
	UBYTE        done;
} MidiTrack;

typedef struct {
	UBYTE     *data;
	ULONG      size;
	MidiTrack  trk[MIDI_MAX_TRACKS];
	int        ntrk;
	ULONG      division;   /* ticks per quarter note */
	ULONG      tempo;      /* microseconds per quarter note */
	ULONG      now_tick;
	ULONG      pend_us;    /* time merged away below MIDI_MERGE_US, still owed */
	UBYTE      chan_vol[16];
} MidiSong;

static MidiSong g_song;

static ULONG Be32(const UBYTE *p)
{
	return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static ULONG Be16(const UBYTE *p)
{
	return ((ULONG)p[0] << 8) | (ULONG)p[1];
}

/* MIDI's variable-length quantity: seven bits per byte, high bit means
 * "another byte follows". Bounded, so a truncated file cannot walk off the
 * end of the buffer. */
static ULONG ReadVar(const UBYTE **pp, const UBYTE *end)
{
	ULONG v = 0;
	int n = 0;
	const UBYTE *p = *pp;

	while (p < end && n < 4) {
		UBYTE b = *p++;
		n++;
		v = (v << 7) | (ULONG)(b & 0x7F);
		if ((b & 0x80) == 0) break;
	}
	*pp = p;
	return v;
}

static void SongFree(MidiSong *s)
{
	if (s->data != NULL) FreeVec(s->data);
	s->data = NULL;
	s->ntrk = 0;
}

/* Read the whole file in one go. Loading on a real Amiga is dominated by the
 * NUMBER of disk requests (see amiga.fast_load), and a song is small. */
static UBYTE *LoadWhole(const char *path, ULONG *out_size)
{
	BPTR   fh;
	UBYTE *buf;
	LONG   size, got;

	fh = Open((STRPTR)path, MODE_OLDFILE);
	if (fh == 0) return NULL;

	if (Seek(fh, 0, OFFSET_END) < 0) { Close(fh); return NULL; }
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if (size <= 14 || (ULONG)size > MIDI_MAX_FILE) { Close(fh); return NULL; }

	buf = (UBYTE *)AllocVec((ULONG)size, MEMF_ANY);
	if (buf == NULL) { Close(fh); return NULL; }

	got = Read(fh, buf, size);
	Close(fh);
	if (got != size) { FreeVec(buf); return NULL; }

	*out_size = (ULONG)size;
	return buf;
}

/* Split a Standard MIDI File into its tracks. Formats 0, 1 and 2 all consist
 * of MThd followed by MTrk chunks, so one walk handles them all; format 2's
 * tracks are meant to be separate songs, but merging them is better than
 * refusing to play. */
static int SongLoad(MidiSong *s, const char *path)
{
	const UBYTE *p, *end;
	ULONG div;
	int i;

	SongFree(s);
	memset(s, 0, sizeof(*s));

	s->data = LoadWhole(path, &s->size);
	if (s->data == NULL) { g_error = "cannot read the file"; return 0; }

	p   = s->data;
	end = s->data + s->size;

	if (memcmp(p, "MThd", 4) != 0) { SongFree(s); g_error = "not a MIDI file"; return 0; }
	if (Be32(p + 4) < 6)           { SongFree(s); g_error = "broken MIDI header"; return 0; }

	div = Be16(p + 12);
	if ((div & 0x8000) != 0) {
		/* SMPTE timing: the high byte is a negative frame rate, the low byte
		 * ticks per frame. Nothing we ship uses it, and converting it would be
		 * a guess, so refuse rather than play the song at the wrong speed. */
		SongFree(s);
		g_error = "SMPTE-timed MIDI is not supported";
		return 0;
	}
	if (div == 0) div = 96;
	s->division = div;
	s->tempo    = 500000;   /* 120 bpm until the file says otherwise */

	p = s->data + 8 + Be32(s->data + 4);
	while (p + 8 <= end && s->ntrk < MIDI_MAX_TRACKS) {
		ULONG len = Be32(p + 4);
		if (memcmp(p, "MTrk", 4) == 0) {
			const UBYTE *tp = p + 8;
			const UBYTE *te = tp + len;
			if (te > end || te < tp) te = end;   /* truncated last track: play what is there */
			s->trk[s->ntrk].p       = tp;
			s->trk[s->ntrk].end     = te;
			s->trk[s->ntrk].running = 0;
			s->trk[s->ntrk].done    = 0;
			s->trk[s->ntrk].next_tick = ReadVar(&s->trk[s->ntrk].p, te);
			s->ntrk++;
		}
		if (len > (ULONG)(end - p - 8)) break;
		p += 8 + len;
	}

	if (s->ntrk == 0) { SongFree(s); g_error = "MIDI file has no tracks"; return 0; }

	for (i = 0; i < 16; i++) s->chan_vol[i] = 100;   /* the GM default */
	s->now_tick = 0;
	return 1;
}

/* ------------------------------------------------------------- CAMD output */

static void Put3(UBYTE status, UBYTE d1, UBYTE d2)
{
	if (g_midi_link == NULL) return;
	PutMidi(g_midi_link, (LONG)(((ULONG)status << 24) | ((ULONG)d1 << 16) | ((ULONG)d2 << 8)));
}

/* Channel volume, scaled by OpenTTD's slider. Sending CC 7 rather than
 * rewriting note velocities means a song that fades itself still fades. */
static void SendChannelVolume(int ch)
{
	long v = (long)g_song.chan_vol[ch] * (long)g_volume / 127;
	if (v > 127) v = 127;
	if (v < 0)   v = 0;
	Put3((UBYTE)(0xB0 | ch), 7, (UBYTE)v);
}

static void AllNotesOff(void)
{
	int ch;
	for (ch = 0; ch < 16; ch++) {
		Put3((UBYTE)(0xB0 | ch), 120, 0);   /* all sound off */
		Put3((UBYTE)(0xB0 | ch), 123, 0);   /* all notes off */
	}
}

static void ResetForNewSong(void)
{
	int ch;
	AllNotesOff();
	for (ch = 0; ch < 16; ch++) {
		Put3((UBYTE)(0xB0 | ch), 121, 0);   /* reset all controllers */
		g_song.chan_vol[ch] = 100;
		SendChannelVolume(ch);
	}
}

/* --------------------------------------------------------- the event pump */

/* Emit every event due at the current tick and return how many microseconds
 * to wait for the next one, or -1 once the song has ended. */
static LONG StepSong(MidiSong *s)
{
	for (;;) {
		int   best = -1;
		ULONG best_tick = 0;
		int   i;
		MidiTrack *t;
		UBYTE status;

		for (i = 0; i < s->ntrk; i++) {
			if (s->trk[i].done) continue;
			if (best < 0 || s->trk[i].next_tick < best_tick) {
				best = i;
				best_tick = s->trk[i].next_tick;
			}
		}
		if (best < 0) return -1;          /* every track has ended */

		if (best_tick > s->now_tick) {
			/* Nothing more to do at this instant, so work out the wait. The
			 * delta is capped so that a file with an absurd one cannot
			 * overflow the microsecond count. */
			ULONG dt = best_tick - s->now_tick;
			ULONG wait;

			/* dt * tempo / division, without either the truncation of a
			 * per-tick constant or a 32-bit overflow. The quotient part is
			 * exact; the remainder part is bounded by 60000 * 32767, which
			 * still fits. Doing it as tempo/division first loses 0.06% per
			 * tick at the usual 500000/480, and that adds up over a song. */
			if (dt > 60000UL) dt = 60000UL;
			wait = dt * (s->tempo / s->division) +
					(dt * (s->tempo % s->division)) / s->division;
			s->now_tick = best_tick;

			/* Gaps too short to be worth a timer request are merged - but the
			 * time is CARRIED, not dropped. Discarding it cost 4% of the
			 * running time (a 103.3 s theme played in 99.4 s), because at
			 * 480 ticks per quarter almost every one- or two-tick gap is under
			 * the threshold and there are thousands of them. */
			s->pend_us += wait;
			if (s->pend_us < MIDI_MERGE_US) continue;
			wait = s->pend_us;
			s->pend_us = 0;
			return (LONG)wait;
		}

		t = &s->trk[best];
		if (t->p >= t->end) { t->done = 1; continue; }

		status = *t->p;
		if (status & 0x80) {
			t->p++;
			if (status < 0xF0) t->running = status;
		} else {
			status = t->running;
			if (status == 0) { t->done = 1; continue; }   /* garbage - drop the track */
		}

		if (status == 0xFF) {
			/* Meta event: only tempo and end-of-track mean anything to us. */
			UBYTE type;
			ULONG len;
			if (t->p >= t->end) { t->done = 1; continue; }
			type = *t->p++;
			len  = ReadVar(&t->p, t->end);
			if (len > (ULONG)(t->end - t->p)) len = (ULONG)(t->end - t->p);
			if (type == 0x2F) { t->done = 1; continue; }
			if (type == 0x51 && len >= 3) {
				ULONG tempo = ((ULONG)t->p[0] << 16) | ((ULONG)t->p[1] << 8) | (ULONG)t->p[2];
				if (tempo != 0) s->tempo = tempo;
			}
			t->p += len;
		} else if (status == 0xF0 || status == 0xF7) {
			/* System exclusive. We do not forward it: a GM reset from the file
			 * would be welcome, but a dump aimed at whatever synth the song was
			 * written for is not, and telling the two apart is more trouble
			 * than it is worth. */
			ULONG len = ReadVar(&t->p, t->end);
			if (len > (ULONG)(t->end - t->p)) len = (ULONG)(t->end - t->p);
			t->p += len;
		} else {
			UBYTE d1 = 0, d2 = 0;
			UBYTE hi = (UBYTE)(status & 0xF0);
			int   ch = status & 0x0F;
			int   two = (hi != 0xC0 && hi != 0xD0);

			if (t->p < t->end) d1 = *t->p++;
			if (two && t->p < t->end) d2 = *t->p++;

			if (hi == 0xB0 && d1 == 7) {
				/* The song setting a channel's volume. Remember it and send our
				 * scaled version instead, so the slider keeps working. */
				s->chan_vol[ch] = d2;
				SendChannelVolume(ch);
			} else {
				Put3(status, d1, d2);
			}
		}

		if (t->p >= t->end) {
			t->done = 1;
		} else {
			t->next_tick = s->now_tick + ReadVar(&t->p, t->end);
		}
	}
}

/* ------------------------------------------------------ the player process */

#define SIG_CMD  SIGBREAKF_CTRL_F

static void PlayerProc(void)
{
	struct MsgPort     *tport = NULL;
	struct timerequest *treq  = NULL;
	int   have_device = 0;
	int   timer_running = 0;
	LONG  vol_seen = 0;
	ULONG timer_sig = 0;

	tport = CreateMsgPort();
	if (tport != NULL) {
		treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
	}
	if (treq != NULL && OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
			(struct IORequest *)treq, 0) == 0) {
		have_device = 1;
		timer_sig = 1UL << tport->mp_SigBit;
	}

	if (!have_device) {
		g_started = -1;
		Signal(g_starter, SIGF_SINGLE);
		if (treq  != NULL) DeleteIORequest((struct IORequest *)treq);
		if (tport != NULL) DeleteMsgPort(tport);
		return;
	}

	g_started = 1;
	Signal(g_starter, SIGF_SINGLE);

	for (;;) {
		ULONG sigs;

		if (timer_running) {
			sigs = Wait(SIG_CMD | SIGBREAKF_CTRL_C | timer_sig);
			if ((sigs & timer_sig) == 0) AbortIO((struct IORequest *)treq);
			WaitIO((struct IORequest *)treq);
			SetSignal(0, timer_sig);
			timer_running = 0;
		} else {
			sigs = Wait(SIG_CMD | SIGBREAKF_CTRL_C);
		}

		if (g_quit || (sigs & SIGBREAKF_CTRL_C) != 0) break;

		/* A new request? Copy it out under Forbid so it cannot change while we
		 * are reading it. */
		if (g_done_seq != g_req_seq) {
			char path[MIDI_PATH_MAX];
			LONG kind;

			Forbid();
			kind = g_req_kind;
			strcpy(path, g_req_path);
			g_done_seq = g_req_seq;
			Permit();

			AllNotesOff();
			SongFree(&g_song);
			g_playing = 0;

			if (kind == 1) {
				if (SongLoad(&g_song, path)) {
					ResetForNewSong();
					g_playing = 1;
				} else {
					MidiLog(g_error);
				}
			}
		}

		if (vol_seen != g_vol_seq) {
			vol_seen = g_vol_seq;
			if (g_playing) {
				int ch;
				for (ch = 0; ch < 16; ch++) SendChannelVolume(ch);
			}
		}

		if (g_playing) {
			LONG wait = StepSong(&g_song);
			if (wait < 0) {
				AllNotesOff();
				g_playing = 0;          /* OpenTTD polls this and moves on */
			} else {
				treq->tr_node.io_Command = TR_ADDREQUEST;
				treq->tr_time.tv_secs    = (ULONG)wait / 1000000UL;
				treq->tr_time.tv_micro   = (ULONG)wait % 1000000UL;
				SendIO((struct IORequest *)treq);
				timer_running = 1;
			}
		}
	}

	if (timer_running) {
		AbortIO((struct IORequest *)treq);
		WaitIO((struct IORequest *)treq);
	}
	AllNotesOff();
	SongFree(&g_song);
	CloseDevice((struct IORequest *)treq);
	DeleteIORequest((struct IORequest *)treq);
	DeleteMsgPort(tport);

	g_player = NULL;
	Signal(g_starter, SIGF_SINGLE);   /* Shutdown() is waiting for this */
}

/* -------------------------------------------------------------- public API */

const char *AmigaMidi_LastError(void)
{
	return g_error;
}

int AmigaMidi_Start(void)
{
	struct Process *proc;

	if (g_player != NULL) return 1;

	g_error = "";
	CamdBase = OpenLibrary((STRPTR)"camd.library", 0);
	if (CamdBase == NULL) {
		g_error = "camd.library is not installed";
		MidiLog("camd.library not found - MIDI unavailable");
		return 0;
	}

	g_midi_node = CreateMidi(MIDI_Name, (Tag)"OpenTTD",
			MIDI_ClientType, CCType_Sequencer,
			TAG_END);
	if (g_midi_node == NULL) {
		g_error = "CreateMidi failed";
		MidiLog("CreateMidi failed");
		CloseLibrary(CamdBase);
		CamdBase = NULL;
		return 0;
	}

	/* "out.0" is the first MIDI output port as the MidiPorts preferences
	 * program names it. If the user has routed nothing there the link still
	 * succeeds and the notes go nowhere - a setup problem, not ours. */
	g_midi_link = AddMidiLink(g_midi_node, MLTYPE_Sender,
			MLINK_Name,     (Tag)"OpenTTD.out",
			MLINK_Location, (Tag)"out.0",
			TAG_END);
	if (g_midi_link == NULL) {
		g_error = "no MIDI output cluster (out.0)";
		MidiLog("AddMidiLink to out.0 failed");
		DeleteMidi(g_midi_node);
		g_midi_node = NULL;
		CloseLibrary(CamdBase);
		CamdBase = NULL;
		return 0;
	}

	g_starter = FindTask(NULL);
	g_started = 0;
	g_quit    = 0;
	SetSignal(0, SIGF_SINGLE);

	proc = CreateNewProcTags(
			NP_Entry,     (Tag)PlayerProc,
			NP_Name,      (Tag)"OpenTTD MIDI",
			NP_StackSize, 16384,
			NP_Priority,  5,          /* above the game: it only ever runs briefly */
			TAG_END);
	if (proc == NULL) {
		g_error = "cannot start the MIDI player process";
		MidiLog("CreateNewProc failed");
		RemoveMidiLink(g_midi_link); g_midi_link = NULL;
		DeleteMidi(g_midi_node);     g_midi_node = NULL;
		CloseLibrary(CamdBase);      CamdBase = NULL;
		return 0;
	}

	g_player = &proc->pr_Task;
	Wait(SIGF_SINGLE);

	if (g_started != 1) {
		g_error = "the MIDI player could not open timer.device";
		MidiLog("player process failed to open timer.device");
		g_player = NULL;
		RemoveMidiLink(g_midi_link); g_midi_link = NULL;
		DeleteMidi(g_midi_node);     g_midi_node = NULL;
		CloseLibrary(CamdBase);      CamdBase = NULL;
		return 0;
	}

	MidiLog("camd.library opened, sending to cluster out.0");
	return 1;
}

void AmigaMidi_Shutdown(void)
{
	if (g_player != NULL) {
		g_starter = FindTask(NULL);
		SetSignal(0, SIGF_SINGLE);
		g_quit = 1;
		Signal(g_player, SIGBREAKF_CTRL_C);
		Wait(SIGF_SINGLE);
	}
	if (g_midi_link != NULL) { RemoveMidiLink(g_midi_link); g_midi_link = NULL; }
	if (g_midi_node != NULL) { DeleteMidi(g_midi_node);     g_midi_node = NULL; }
	if (CamdBase    != NULL) { CloseLibrary(CamdBase);      CamdBase    = NULL; }
}

int AmigaMidi_Play(const char *path)
{
	if (g_player == NULL || path == NULL || path[0] == '\0') return 0;
	if (strlen(path) >= MIDI_PATH_MAX) return 0;

	Forbid();
	strcpy(g_req_path, path);
	g_req_kind = 1;
	g_req_seq++;
	Permit();

	Signal(g_player, SIG_CMD);
	return 1;
}

void AmigaMidi_Stop(void)
{
	if (g_player == NULL) return;

	Forbid();
	g_req_kind = 0;
	g_req_seq++;
	Permit();

	Signal(g_player, SIG_CMD);
}

int AmigaMidi_IsPlaying(void)
{
	/* A request the player has not picked up yet counts as playing, otherwise
	 * OpenTTD sees "finished" in the gap between Play() and the process waking
	 * up, and skips straight on to the next track. */
	if (g_player == NULL) return 0;
	if (g_done_seq != g_req_seq) return g_req_kind == 1;
	return g_playing != 0;
}

void AmigaMidi_SetVolume(int vol)
{
	if (vol < 0)   vol = 0;
	if (vol > 127) vol = 127;
	g_volume = vol;
	g_vol_seq++;
	if (g_player != NULL) Signal(g_player, SIG_CMD);
}
