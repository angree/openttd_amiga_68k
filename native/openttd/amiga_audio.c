/* Paula audio for OpenTTD on AmigaOS 68k - OS layer implementation.
 *
 * Plain C, the only file in the sound path that includes Amiga headers
 * (they collide with OpenTTD's C++ - same rule as amiga_gfx.c).
 *
 * Every pattern here was proven standalone in native/paulatest.c:
 *  - OpenDevice("audio.device") with io_Length != 0 performs ADCMD_ALLOCATE;
 *    mask 15 = all four channels, ADIOF_NOWAIT = fail fast instead of
 *    blocking if another program holds Paula.
 *  - AUDIO.DEVICE RULE (cost a day of silence): requests MUST be sent with
 *    BeginIO(). SendIO() and DoIO() clear io_Flags, which wipes ADIOF_PERVOL;
 *    the device then keeps the channel's previous volume - 0 right after
 *    allocation - and every write "succeeds" inaudibly with io_Error = 0.
 *  - Completion is polled with CheckIO() and the replied request dequeued
 *    with WaitIO() (which does not block once CheckIO returned non-NULL).
 *    The game is built thread_none: nothing here ever blocks.
 *  - Sample data must be Chip RAM (Paula DMA reads nothing else),
 *    word-aligned (AllocVec guarantees that), even length.
 *
 * Build by hand, same ABI as the rest of the game, and at -O0 - this
 * toolchain has a record of miscompiling read-after-call patterns at -O1:
 *   m68k-amigaos-gcc -O0 -m68040 -msoft-float -noixemul \
 *       -c amiga_audio.c -o amiga_audio.o
 * The object is linked via LIBS in objs/release/Makefile, like amiga_gfx.o.
 */

#include <proto/exec.h>
#include <clib/alib_protos.h>   /* BeginIO - amiga.lib stub (-lamiga) */
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/audio.h>

#include "amiga_audio.h"

/* Paula length register counts 16-bit words: max 65535 words = 131070 bytes. */
#define AA_MAX_BYTES 131070UL

static struct MsgPort *aa_port = NULL;
/* Opener request: owns the four-channel allocation (and the AllocKey). */
static struct IOAudio *aa_opener = NULL;
/* One write request per channel, cloned from the opener. */
static struct IOAudio *aa_req[AMIGA_AUDIO_CHANNELS];
static int aa_busy[AMIGA_AUDIO_CHANNELS];   /* CMD_WRITE outstanding? */
static int aa_devopen = 0;
static int aa_ready = 0;

/* The only allocation we accept: all four channels at once. */
static UBYTE aa_allocmap[] = { 15 };

int AmigaAudio_Open(void)
{
    int i;

    if (aa_ready) return 1;

    aa_port = CreateMsgPort();
    if (aa_port == NULL) goto fail;

    aa_opener = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (aa_opener == NULL) goto fail;

    aa_opener->ioa_Request.io_Message.mn_ReplyPort   = aa_port;
    aa_opener->ioa_Request.io_Message.mn_Node.ln_Pri = 0;  /* precedence */
    aa_opener->ioa_Request.io_Message.mn_Length      = sizeof(struct IOAudio);
    /* io_Length != 0 makes OpenDevice do an ADCMD_ALLOCATE with this map;
     * ADIOF_NOWAIT = fail instead of blocking if channels are taken. */
    aa_opener->ioa_Request.io_Flags = ADIOF_NOWAIT;
    aa_opener->ioa_Data   = aa_allocmap;
    aa_opener->ioa_Length = sizeof(aa_allocmap);

    if (OpenDevice((STRPTR)AUDIONAME, 0,
                   (struct IORequest *)aa_opener, 0) != 0) {
        goto fail;   /* channels busy (-11) or no device: run silent */
    }
    aa_devopen = 1;

    /* Clone the opener per channel: copies io_Device, the reply port and,
     * critically, ioa_AllocKey - the proof of ownership every later command
     * must carry. io_Unit is narrowed to the one channel per request. */
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        aa_req[i] = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                               MEMF_PUBLIC | MEMF_CLEAR);
        if (aa_req[i] == NULL) goto fail;
        *aa_req[i] = *aa_opener;
        aa_req[i]->ioa_Request.io_Unit = (struct Unit *)(1UL << i);
        aa_busy[i] = 0;
    }

    aa_ready = 1;
    return 1;

fail:
    AmigaAudio_Close();
    return 0;
}

void AmigaAudio_Close(void)
{
    int i;

    aa_ready = 0;

    /* Abort and dequeue any outstanding write before freeing its request. */
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        if (aa_req[i] != NULL && aa_busy[i]) {
            AbortIO((struct IORequest *)aa_req[i]);
            WaitIO((struct IORequest *)aa_req[i]);
            aa_busy[i] = 0;
        }
    }

    if (aa_devopen) {
        /* Explicit ADCMD_FREE (resets + frees our channels), then
         * CloseDevice - belt and braces, exactly as in paulatest.c. */
        aa_opener->ioa_Request.io_Command = ADCMD_FREE;
        aa_opener->ioa_Request.io_Flags   = 0;  /* reply via port */
        aa_opener->ioa_Request.io_Unit    = (struct Unit *)15;
        BeginIO((struct IORequest *)aa_opener);  /* never DoIO/SendIO */
        WaitIO((struct IORequest *)aa_opener);
        CloseDevice((struct IORequest *)aa_opener);
        aa_devopen = 0;
    }

    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        if (aa_req[i] != NULL) { FreeVec(aa_req[i]); aa_req[i] = NULL; }
    }
    if (aa_opener != NULL) { FreeVec(aa_opener); aa_opener = NULL; }
    if (aa_port != NULL) { DeleteMsgPort(aa_port); aa_port = NULL; }
}

void *AmigaAudio_AllocSample(unsigned long bytes)
{
    if (bytes == 0) return NULL;
    return AllocVec(bytes, MEMF_CHIP);
}

void AmigaAudio_FreeSample(void *p)
{
    if (p != NULL) FreeVec(p);
}

int AmigaAudio_ChannelIdle(int ch)
{
    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (!aa_busy[ch]) return 1;
    if (CheckIO((struct IORequest *)aa_req[ch]) != NULL) {
        /* Replied: WaitIO just dequeues, it cannot block here. */
        WaitIO((struct IORequest *)aa_req[ch]);
        aa_busy[ch] = 0;
        return 1;
    }
    return 0;
}

int AmigaAudio_Play(int ch, void *chipdata, unsigned long bytes,
                    int period, int volume)
{
    struct IOAudio *io;

    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (chipdata == NULL || bytes < 2) return 0;
    if (!AmigaAudio_ChannelIdle(ch)) return 0;

    if (bytes > AA_MAX_BYTES) bytes = AA_MAX_BYTES;
    if (period < 124)   period = 124;    /* PAL DMA floor (~28.6 kHz) */
    if (period > 65535) period = 65535;
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;

    io = aa_req[ch];
    io->ioa_Request.io_Command = CMD_WRITE;
    io->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io->ioa_Request.io_Unit    = (struct Unit *)(1UL << ch);
    io->ioa_Data   = (UBYTE *)chipdata;
    io->ioa_Length = bytes & ~1UL;       /* Paula wants an even byte count */
    io->ioa_Period = (UWORD)period;
    io->ioa_Volume = (UWORD)volume;
    io->ioa_Cycles = 1;                  /* play once, then the channel stops */

    /* MUST be BeginIO - see the file header. */
    BeginIO((struct IORequest *)io);
    aa_busy[ch] = 1;
    return 1;
}
