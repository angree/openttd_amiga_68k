/* Milestone 1 of Amiga sound for OpenTTD: prove Paula audio works, standalone.
 *
 * Two parts:
 *
 * A) One-shot diagnostics (a single run still yields all measurements):
 *   1. allocates all four channels via OpenDevice/ADCMD_ALLOCATE (no register
 *      banging), with ADIOF_NOWAIT so a busy system fails fast instead of
 *      hanging,
 *   2. plays a 1-second tone on each channel in turn, logging which speaker
 *      it should come out of (Paula routing: channels 0+3 -> LEFT jack,
 *      1+2 -> RIGHT jack; MOD players pan L-R-R-L),
 *   3. plays the same tone at hardware volumes 64/32/16/4,
 *   4. sweeps the period register down past the PAL DMA floor (~124).
 *
 * B) Endless beacon loop (walk-away test): a ~1 s 440 Hz tone every ~10 s,
 *    forever, rotating through channels 0-3 so channel + stereo routing can
 *    be confirmed by ear at any time. One log line per iteration (counter,
 *    channel, speaker, measured vs expected ms and ratio), appended to
 *    Work:paulaloop.log with an Open/FPrintf/Close per line so the host sees
 *    it while the emulator keeps running. Stop with Ctrl-C: channels are
 *    freed and the device closed so the next run's allocation succeeds.
 *
 * TIMING: duration of one CMD_WRITE is
 *      seconds = ioa_Cycles * samples_per_cycle * ioa_Period / 3546895 (PAL)
 * i.e. Paula fetches (PAL_CLOCK / period) 8-bit samples per second and
 * ioa_Cycles counts repetitions of the ioa_Length-sample waveform. In an
 * earlier run every tone "completed" ~8x early with time proportional to
 * ioa_Cycles but INDEPENDENT of ioa_Period (effective rate pinned at
 * ~58-62 kHz): that is WinUAE with no sound_output configured servicing
 * audio DMA without cycle-accurate period pacing - an emulator artifact,
 * not a formula error. Note also that audio.device may reply up to one
 * waveform cycle early (Paula double-buffers pointer/length); at 440 Hz
 * that is ~2.3 ms - harmless, but remember it when the game uses write
 * completion to mark a channel free.
 *
 * CLOCK: all durations are measured with timer.device ReadEClock() - the
 *   PAL E-clock, 709379 ticks/s (~1.41 us resolution), safe from a normal
 *   task once timer.device is open. Earlier revisions counted Delay(1)
 *   poll iterations and assumed 20 ms each, which under-reported elapsed
 *   time whenever Delay(1) blocked for more than one tick; section A0
 *   calibrates Delay(1) against the E-clock to expose exactly that.
 *   Section A4 measures how early audio.device replies relative to the
 *   nominal duration (busy-polled, so detection latency is microseconds)
 *   and whether a channel restarted the instant its write completes loses
 *   any time (back-to-back total vs sum of nominal durations).
 *
 * All completion is non-blocking: BeginIO + CheckIO + Delay(1) polling,
 * never Wait() - the pattern the thread-less game will use. Every poll
 * loop has a timeout escape (AbortIO) and a Ctrl-C escape.
 *
 * AUDIO.DEVICE RULE (cost a day of silence): requests MUST be sent with
 * BeginIO(). SendIO() and DoIO() clear io_Flags, which wipes ADIOF_PERVOL;
 * the device then "uses the previous volume and period for that channel",
 * which after allocation is volume 0 - every write completes with
 * io_Error=0 and plays nothing, and durations do not track ioa_Period.
 * (wiki.amigaos.net "Audio Device".)
 *
 * Waveform: one 32-sample sine cycle, 8-bit signed, integer table (no
 * floating point anywhere), copied into Chip RAM (Paula DMA reads Chip
 * RAM only).
 *
 * Build (Windows toolchain, Git Bash - /usr/bin first so cp is not shadowed;
 * two steps: the one-step compile-and-link fails on this toolchain):
 *   export PATH=/usr/bin:/bin:/c/amiga-gcc/bin
 *   cd native && m68k-amigaos-gcc -m68040 -O1 -noixemul -c -o paulatest.o paulatest.c
 *             && m68k-amigaos-gcc -m68040 -O1 -noixemul -o paulatest paulatest.o
 * Run on the Amiga:
 *   Work:paulatest >Work:paulatest.log
 * (preamble + a copy of each loop line go to stdout; the loop also appends
 *  to Work:paulaloop.log, which is deleted at program start for a clean run)
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>   /* BeginIO - amiga.lib stub, not an exec call */
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/audio.h>
#include <devices/timer.h>
#include <proto/timer.h>        /* ReadEClock - direct call via TimerBase */
#include <dos/dos.h>

unsigned long __stack = 64 * 1024;

/* PAL Paula clock: sample rate = PAL_CLOCK / period. */
#define PAL_CLOCK     3546895UL
#define WAVE_SAMPLES  32         /* one sine cycle; even, word-aligned by AllocVec */

/* Beacon loop: period 252 -> 14074 samples/s -> ~439.8 Hz tone; 440 cycles
 * of it last ~1.0 s. Full hardware volume, ~9 s gap -> ~10 s per iteration. */
#define LOOP_PERIOD    252
#define LOOP_CYCLES    440
#define LOOP_VOLUME    64
#define LOOP_GAP_TICKS 450
#define LOOP_LOG       "Work:paulaloop.log"

/* One cycle of sine, 127*sin(2*pi*i/32), precomputed - integers only. */
static const BYTE sine32[WAVE_SAMPLES] = {
      0,  25,  49,  71,  90, 106, 117, 125,
    127, 125, 117, 106,  90,  71,  49,  25,
      0, -25, -49, -71, -90,-106,-117,-125,
   -127,-125,-117,-106, -90, -71, -49, -25
};

/* The only allocation we accept: all four channels at once (mask 0x0F). */
static UBYTE allocmap[] = { 15 };

/* Paula stereo routing by channel number. */
static const char *side[4] = { "LEFT", "RIGHT", "RIGHT", "LEFT" };

static struct MsgPort *port  = NULL;
static struct IOAudio *ioa   = NULL;
static BYTE  *chipwave       = NULL;
static BOOL   devopen        = FALSE;

/* timer.device, opened only for ReadEClock() - our measurement clock. */
struct Device *TimerBase     = NULL;   /* name/type required by proto/timer.h */
static struct MsgPort      *tport     = NULL;
static struct timerequest  *treq      = NULL;
static BOOL   timeropen               = FALSE;
static ULONG  efreq                   = 0;   /* EClock ticks/s (~709379 PAL) */

/* Ctrl-C arrived since last check? (checking clears it) */
static BOOL ctrlc(void)
{
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* Milliseconds between two EClock readings. The E-clock is a 64-bit counter
 * at efreq (~709379) ticks/s; unsigned 32-bit subtraction of the low words
 * is exact for any span under ~6053 s, which covers everything here. */
static ULONG ms_between(const struct EClockVal *a, const struct EClockVal *b)
{
    ULONG diff = b->ev_lo - a->ev_lo;
    return (ULONG)((unsigned long long)diff * 1000ULL / efreq);
}

/* Expected duration of one CMD_WRITE in milliseconds (see TIMING above). */
static ULONG expect_ms(UWORD period, UWORD cycles)
{
    return (ULONG)((unsigned long long)cycles * WAVE_SAMPLES * period
                   * 1000ULL / PAL_CLOCK);
}

/* Print one measurement line: measured vs expected ms plus their ratio. */
static void report_ms(LONG meas, ULONG exp)
{
    ULONG r = exp ? (((ULONG)meas * 100UL + exp / 2) / exp) : 0;
    Printf("  done: measured %ld ms, expected %lu ms, ratio %lu.%02lu\n",
           meas, exp, r / 100, r % 100);
}

/* Play one tone on `chmask` and poll it to completion without blocking.
 * Completion is detected exactly as the game will: CheckIO(), never Wait().
 * busy=FALSE: Delay(1) between polls (the game's idle pattern; detection
 *             granularity = however long Delay(1) really blocks).
 * busy=TRUE : tight CheckIO loop - measurement-only mode with microsecond
 *             detection latency, used to quantify early replies in A4.
 * Returns measured EClock milliseconds (>= 0), -1 on error/timeout, -2 on
 * Ctrl-C. Time runs from just before BeginIO to CheckIO turning non-NULL. */
static LONG play2(ULONG chmask, UWORD period, UWORD vol, UWORD cycles,
                  BOOL busy)
{
    struct EClockVal t0, t1;
    ULONG limit = expect_ms(period, cycles) + 5000;    /* 5 s slack */

    ioa->ioa_Request.io_Command = CMD_WRITE;
    ioa->ioa_Request.io_Flags   = ADIOF_PERVOL;
    ioa->ioa_Request.io_Unit    = (struct Unit *)chmask;
    ioa->ioa_Data   = (UBYTE *)chipwave;
    ioa->ioa_Length = WAVE_SAMPLES;
    ioa->ioa_Period = period;
    ioa->ioa_Volume = vol;
    ioa->ioa_Cycles = cycles;

    /* MUST be BeginIO, not SendIO: exec's SendIO()/DoIO() clear io_Flags,
     * wiping ADIOF_PERVOL, so the device never programs period/volume and
     * the channel plays at its post-allocation volume of 0 -> total silence
     * with io_Error=0.  wiki.amigaos.net Audio Device: "You should not use
     * SendIO() or DoIO() with the audio device ... To be safe, you should
     * always use BeginIO()."  IOF_QUICK is NOT set in io_Flags, so the
     * device always replies via the port and CheckIO/WaitIO stay valid. */
    ReadEClock(&t0);
    BeginIO((struct IORequest *)ioa);

    while (CheckIO((struct IORequest *)ioa) == NULL) {
        if (!busy) Delay(1);           /* keeps CPU idle; NOT the clock */
        if (ctrlc()) {
            AbortIO((struct IORequest *)ioa);
            WaitIO((struct IORequest *)ioa);
            return -2;
        }
        ReadEClock(&t1);
        if (ms_between(&t0, &t1) > limit) {
            Printf("  TIMEOUT after %lu ms - AbortIO\n", ms_between(&t0, &t1));
            AbortIO((struct IORequest *)ioa);
            WaitIO((struct IORequest *)ioa);
            return -1;
        }
    }
    ReadEClock(&t1);
    WaitIO((struct IORequest *)ioa);   /* dequeue the already-replied request */
    if (ioa->ioa_Request.io_Error != 0) {
        Printf("  FAIL: io_Error=%ld\n", (LONG)ioa->ioa_Request.io_Error);
        return -1;
    }
    return (LONG)ms_between(&t0, &t1);
}

static LONG play(ULONG chmask, UWORD period, UWORD vol, UWORD cycles)
{
    return play2(chmask, period, vol, cycles, FALSE);
}

/* Sleep `total` ticks in short slices; TRUE if Ctrl-C arrived meanwhile. */
static BOOL gap(LONG total)
{
    LONG t;
    for (t = 0; t < total; t += 5) {
        Delay(5);
        if (ctrlc()) return TRUE;
    }
    return FALSE;
}

int main(void)
{
    ULONG chipBefore, chipLargest, chipAfter;
    ULONG iter;
    LONG  err, i, t;
    BOOL  stopped = FALSE;
    int   rc = 20;

    static const UWORD vols[]    = { 64, 32, 16, 4 };
    /* Period sweep: halving steps down through the PAL DMA floor (~124). */
    static const UWORD periods[] = { 1008, 504, 252, 126, 124, 96, 62 };

    Printf("paulatest: Paula audio via audio.device\n");
    Printf("PAL clock %lu, waveform %ld samples/cycle\n\n",
           PAL_CLOCK, (LONG)WAVE_SAMPLES);

    DeleteFile((STRPTR)LOOP_LOG);      /* fresh per-run loop log */

    chipBefore  = AvailMem(MEMF_CHIP);
    chipLargest = AvailMem(MEMF_CHIP | MEMF_LARGEST);
    Printf("Chip RAM before: %lu free, largest block %lu\n\n",
           chipBefore, chipLargest);

    /* ---- timer.device: ReadEClock() is the measurement clock ------------- */
    tport = CreateMsgPort();
    if (tport == NULL) { Printf("FAIL: CreateMsgPort (timer)\n"); goto done; }
    treq = (struct timerequest *)CreateIORequest(tport,
                                                 sizeof(struct timerequest));
    if (treq == NULL) { Printf("FAIL: CreateIORequest (timer)\n"); goto done; }
    err = OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                     (struct IORequest *)treq, 0);
    if (err != 0) {
        Printf("FAIL: OpenDevice(timer.device) err=%ld\n", err);
        goto done;
    }
    timeropen = TRUE;
    TimerBase = treq->tr_node.io_Device;
    {
        struct EClockVal ev;
        ULONG us100;                       /* tick length in 1/100 us */
        efreq = ReadEClock(&ev);
        us100 = 100000000UL / efreq;
        Printf("OK: timer.device EClock %lu ticks/s (%lu.%02lu us/tick) - clock for all measurements\n\n",
               efreq, us100 / 100, us100 % 100);
    }

    /* ---- open audio.device and allocate all four channels ---------------- */
    port = CreateMsgPort();
    if (port == NULL) { Printf("FAIL: CreateMsgPort\n"); goto done; }

    ioa = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                     MEMF_PUBLIC | MEMF_CLEAR);
    if (ioa == NULL) { Printf("FAIL: AllocVec IOAudio\n"); goto done; }

    ioa->ioa_Request.io_Message.mn_ReplyPort   = port;
    ioa->ioa_Request.io_Message.mn_Node.ln_Pri = 0;   /* allocation precedence */
    ioa->ioa_Request.io_Message.mn_Length      = sizeof(struct IOAudio);
    /* io_Length != 0 makes OpenDevice do an ADCMD_ALLOCATE with this map;
     * ADIOF_NOWAIT = fail instead of blocking if channels are taken. */
    ioa->ioa_Request.io_Flags = ADIOF_NOWAIT;
    ioa->ioa_Data   = allocmap;
    ioa->ioa_Length = sizeof(allocmap);

    err = OpenDevice((STRPTR)AUDIONAME, 0, (struct IORequest *)ioa, 0);
    if (err != 0) {
        Printf("FAIL: OpenDevice(%s) err=%ld (io_Error=%ld; -11=ALLOCFAILED: channels busy)\n",
               (LONG)AUDIONAME, err, (LONG)ioa->ioa_Request.io_Error);
        goto done;
    }
    devopen = TRUE;
    Printf("OK: audio.device open, ADCMD_ALLOCATE got channel mask %ld (want 15), AllocKey=%ld\n\n",
           (LONG)(ULONG)ioa->ioa_Request.io_Unit,
           (LONG)(UWORD)ioa->ioa_AllocKey);

    /* ---- waveform into Chip RAM (Paula DMA reads only Chip RAM) ---------- */
    chipwave = (BYTE *)AllocVec(WAVE_SAMPLES, MEMF_CHIP | MEMF_CLEAR);
    if (chipwave == NULL) { Printf("FAIL: AllocVec %ld bytes CHIP\n", (LONG)WAVE_SAMPLES); goto done; }
    for (i = 0; i < WAVE_SAMPLES; i++) chipwave[i] = sine32[i];
    Printf("OK: %ld-byte sine at $%lx, TypeOfMem=$%lx (MEMF_CHIP=$%lx bit %s)\n\n",
           (LONG)WAVE_SAMPLES, (ULONG)chipwave, TypeOfMem(chipwave),
           (ULONG)MEMF_CHIP,
           (LONG)((TypeOfMem(chipwave) & MEMF_CHIP) ? "SET - DMA ok" : "CLEAR - NOT CHIP!"));

    /* ---- A0: calibrate Delay(1) against the E-clock ----------------------- */
    /* If Delay(1) really blocks ~20 ms, tick-counting was a valid clock; if
     * it blocks longer, every earlier tick-count under-reported elapsed time
     * by exactly that factor. This settles it independently of Paula. */
    {
        struct EClockVal c0, c1;
        ULONG dms;
        ReadEClock(&c0);
        for (i = 0; i < 50; i++) Delay(1);
        ReadEClock(&c1);
        dms = ms_between(&c0, &c1);
        Printf("=== Delay(1) calibration: 50 calls took %lu ms -> %lu.%02lu ms per call (nominal 20.00) ===\n\n",
               dms, dms / 50, (dms % 50) * 2);
    }

    /* ---- A1: each channel in turn, ~220 Hz, 1 s, full volume ------------- */
    Printf("=== channels in turn (period 504 -> rate %lu Hz, tone ~%lu Hz, 1 s each) ===\n",
           PAL_CLOCK / 504, PAL_CLOCK / 504 / WAVE_SAMPLES);
    for (i = 0; i < 4; i++) {
        UWORD cyc = (UWORD)(PAL_CLOCK / 504 / WAVE_SAMPLES);   /* ~219 -> ~1 s */
        Printf("channel %ld - should come out of the %s speaker:\n", i, (LONG)side[i]);
        t = play(1UL << i, 504, 64, cyc);
        if (t == -2) { stopped = TRUE; goto loop_end; }
        if (t < 0) goto done;
        report_ms(t, expect_ms(504, cyc));
        Delay(25);                                    /* half-second gap */
    }
    Printf("\n");

    /* ---- A2: hardware volume levels on channel 0 ------------------------- */
    Printf("=== hardware volume on channel 0 (register range 0-64, no CPU scaling) ===\n");
    for (i = 0; i < 4; i++) {
        UWORD cyc = (UWORD)(PAL_CLOCK / 504 / WAVE_SAMPLES * 7 / 10);  /* ~0.7 s */
        Printf("volume %ld:\n", (LONG)vols[i]);
        t = play(1UL << 0, 504, vols[i], cyc);
        if (t == -2) { stopped = TRUE; goto loop_end; }
        if (t < 0) goto done;
        report_ms(t, expect_ms(504, cyc));
        Delay(15);
    }
    Printf("\n");

    /* ---- A3: period sweep through the PAL DMA floor ----------------------- */
    Printf("=== period sweep on channel 0 (PAL DMA floor ~124 = ~28.6 kHz; below it, listen for breakup) ===\n");
    Printf("=== ear check: period 252 should be concert A ~440 Hz; an octave higher (~880 Hz) = Paula really 2x fast ===\n");
    for (i = 0; i < (LONG)(sizeof(periods) / sizeof(periods[0])); i++) {
        ULONG rate = PAL_CLOCK / periods[i];
        ULONG tone = rate / WAVE_SAMPLES;
        UWORD cyc  = (UWORD)(tone / 2);                        /* ~0.5 s */
        Printf("period %ld -> rate %lu Hz, tone ~%lu Hz%s:\n",
               (LONG)periods[i], rate, tone,
               (LONG)(periods[i] < 124 ? "  [BELOW DMA FLOOR]" : ""));
        t = play(1UL << 0, periods[i], 64, cyc);
        if (t == -2) { stopped = TRUE; goto loop_end; }
        if (t < 0) goto done;
        report_ms(t, expect_ms(periods[i], cyc));
        Delay(15);
    }
    Printf("\n");

    /* ---- A4: channel reuse - early replies and immediate restart ---------- */
    /* The game stops one effect and starts another on the same channel all
     * the time. audio.device can reply up to one waveform cycle early
     * (Paula latches pointer/length for the NEXT cycle while the current
     * one plays). Busy-polling makes detection latency microseconds, so
     * expected-minus-measured IS the early-reply margin. */
    Printf("=== channel reuse on channel 0 (busy-poll: detection latency ~us) ===\n");
    Printf("early-reply margin: 4 x 0.5 s tones; one waveform cycle at period 252 is ~2.3 ms:\n");
    for (i = 0; i < 4; i++) {
        ULONG exp = expect_ms(252, 220);                       /* ~0.5 s */
        t = play2(1UL << 0, 252, 64, 220, TRUE);
        if (t == -2) { stopped = TRUE; goto loop_end; }
        if (t < 0) goto done;
        Printf("  tone %ld: measured %ld ms, expected %lu ms, replied %ld ms early\n",
               i, t, exp, (LONG)exp - t);
        Delay(10);
    }
    {
        /* Back-to-back: restart the channel the instant CheckIO says done.
         * If early replies truncated or delayed anything, the total of 8
         * chained tones would drift from 8x the nominal duration. */
        struct EClockVal b0, b1;
        ULONG expTot = 8 * expect_ms(252, 110);                /* 8 x ~0.25 s */
        ULONG tot, r;
        Printf("back-to-back: 8 x 0.25 s tones, each restarted the instant the previous completes:\n");
        ReadEClock(&b0);
        for (i = 0; i < 8; i++) {
            t = play2(1UL << 0, 252, 64, 110, TRUE);
            if (t == -2) { stopped = TRUE; goto loop_end; }
            if (t < 0) goto done;
        }
        ReadEClock(&b1);
        tot = ms_between(&b0, &b1);
        r   = (tot * 100UL + expTot / 2) / expTot;
        Printf("  total measured %lu ms, expected %lu ms, ratio %lu.%02lu (=1.00 -> no dead time, no truncation)\n",
               tot, expTot, r / 100, r % 100);
    }
    Printf("\n");

    /* ---- B: endless beacon loop ------------------------------------------ */
    Printf("=== beacon loop: ~440 Hz (concert A - an octave higher means Paula 2x fast), ~1 s, vol %ld, every ~10 s, channels 0-3 rotating ===\n",
           (LONG)LOOP_VOLUME);
    Printf("=== stop with Ctrl-C; per-iteration lines also appended to %s ===\n",
           (LONG)LOOP_LOG);
    Flush(Output());

    for (iter = 1; ; iter++) {          /* ULONG: wraps after ~4e9 iterations */
        ULONG ch  = (iter - 1) & 3;     /* iteration 1 -> channel 0, ... */
        ULONG exp = expect_ms(LOOP_PERIOD, LOOP_CYCLES);
        ULONG r;
        BPTR  fh;

        t = play(1UL << ch, LOOP_PERIOD, LOOP_VOLUME, LOOP_CYCLES);
        if (t == -2) { stopped = TRUE; break; }
        if (t < 0) goto done;           /* error already logged by play() */
        r = (( (ULONG)t * 100UL) + exp / 2) / exp;

        Printf("iter %lu: ch %lu (%s speaker), measured %ld ms, expected %lu ms, ratio %lu.%02lu\n",
               iter, ch, (LONG)side[ch], t, exp, r / 100, r % 100);
        Flush(Output());

        /* Append-only line to the dedicated loop log, opened and closed per
         * iteration so the host-side file is updated while we keep running. */
        fh = Open((STRPTR)LOOP_LOG, MODE_READWRITE);
        if (fh) {
            Seek(fh, 0, OFFSET_END);
            FPrintf(fh, "iter %lu: ch %lu (%s speaker), measured %ld ms, expected %lu ms, ratio %lu.%02lu\n",
                    iter, ch, (LONG)side[ch], t, exp, r / 100, r % 100);
            Close(fh);
        }

        if (gap(LOOP_GAP_TICKS)) { stopped = TRUE; break; }
    }

loop_end:
    if (stopped) Printf("\nCtrl-C - stopping cleanly\n");
    rc = 0;

done:
    /* ---- free everything, on every path ----------------------------------- */
    if (devopen) {
        /* Explicit ADCMD_FREE (resets + frees our channels), then CloseDevice
         * (which would also free them - belt and braces, and it logs). */
        ioa->ioa_Request.io_Command = ADCMD_FREE;
        ioa->ioa_Request.io_Flags   = 0;   /* no IOF_QUICK: reply via port */
        ioa->ioa_Request.io_Unit    = (struct Unit *)15;
        BeginIO((struct IORequest *)ioa);  /* never DoIO/SendIO on audio.device */
        WaitIO((struct IORequest *)ioa);
        Printf("ADCMD_FREE: io_Error=%ld\n", (LONG)ioa->ioa_Request.io_Error);
        CloseDevice((struct IORequest *)ioa);
        Printf("audio.device closed\n");
    }
    if (chipwave) FreeVec(chipwave);
    if (ioa)      FreeVec(ioa);
    if (port)     DeleteMsgPort(port);
    if (timeropen) { CloseDevice((struct IORequest *)treq); TimerBase = NULL; }
    if (treq)      DeleteIORequest((struct IORequest *)treq);
    if (tport)     DeleteMsgPort(tport);

    chipAfter = AvailMem(MEMF_CHIP);
    Printf("Chip RAM after: %lu free (before: %lu, delta %ld)\n",
           chipAfter, chipBefore, (LONG)(chipBefore - chipAfter));
    Printf(rc == 0 ? "DONE\n" : "FAILED rc=%ld\n", (LONG)rc);
    return rc;
}
