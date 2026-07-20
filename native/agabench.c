/* Step 2 of the native Amiga display engine: MEASURE the chunky->planar cost.
 *
 * Milestone 1 proved a 640x512x8 AGA screen displays our chunky buffer via
 * graphics.library WritePixelArray8(). That call is correct but slow, and we
 * must not guess by how much. This benchmark answers three questions:
 *
 *   1. How long does a FULL 640x512 frame take through WritePixelArray8?
 *   2. How does that scale with rectangle size (i.e. does dirty-rect blitting
 *      alone already buy us a playable frame rate)?
 *   3. How does our own c2p compare against the system one?
 *
 * Only then do we decide whether hand-written 68k assembly is worth it.
 *
 * Build (Windows toolchain):
 *   m68k-amigaos-gcc -m68040 -O2 -noixemul -o agabench agabench.c
 * Run from the directory holding image.raw / image.pal.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <exec/memory.h>
#include <string.h>

#define SCR_W 640
#define SCR_H 512
#define SCR_DEPTH 8

unsigned long __stack = 1024 * 1024;

static UBYTE palette[768];
static UBYTE bitrev[256];          /* bit-reversal LUT, see c2p_rect() */

/* DateStamp gives 1/50 s ticks; every test loops enough to dwarf that. */
static ULONG now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* ---------------------------------------------------------------------------
 * Our own chunky-to-planar.
 *
 * Each group of 8 horizontal pixels is an 8x8 bit matrix (8 pixels x 8 planes)
 * that has to be transposed: output byte of plane c holds one bit per pixel.
 * Transposing with a per-bit loop costs 64 operations per group; the classic
 * three-stage butterfly below does it in about 25, working on two 32-bit words.
 *
 * Loading the source as two big-endian ULONGs is free on 68k but leaves each
 * output byte holding pixel 0 in bit 0, while planar wants the leftmost pixel
 * in bit 7 - hence the bit-reversal LUT on store.
 * -------------------------------------------------------------------------*/
static void c2p_rect(const UBYTE *src, ULONG srcMod,
                     UBYTE **planes, ULONG bpr,
                     ULONG x, ULONG y, ULONG w, ULONG h)
{
    ULONG row, col;

    for (row = 0; row < h; row++) {
        const ULONG *s = (const ULONG *)(src + row * srcMod);
        ULONG        off = (y + row) * bpr + (x >> 3);

        for (col = 0; col < w; col += 8) {
            ULONG a = *s++;          /* pixels 0..3 */
            ULONG b = *s++;          /* pixels 4..7 */
            ULONG t;

            /* stage 1: swap the two off-diagonal 4x4 quadrants (across words) */
            t = ((a >> 4) ^ b) & 0x0f0f0f0fUL;
            b ^= t;
            a ^= (t << 4);

            /* stage 2: swap off-diagonal 2x2 blocks inside each 4x4 */
            t = (a ^ (a >> 18)) & 0x00003333UL;  a ^= t ^ (t << 18);
            t = (b ^ (b >> 18)) & 0x00003333UL;  b ^= t ^ (t << 18);

            /* stage 3: swap single off-diagonal bits inside each 2x2 */
            t = (a ^ (a >> 9))  & 0x00550055UL;  a ^= t ^ (t << 9);
            t = (b ^ (b >> 9))  & 0x00550055UL;  b ^= t ^ (t << 9);

            planes[0][off] = bitrev[(a >> 24) & 0xff];
            planes[1][off] = bitrev[(a >> 16) & 0xff];
            planes[2][off] = bitrev[(a >>  8) & 0xff];
            planes[3][off] = bitrev[ a        & 0xff];
            planes[4][off] = bitrev[(b >> 24) & 0xff];
            planes[5][off] = bitrev[(b >> 16) & 0xff];
            planes[6][off] = bitrev[(b >>  8) & 0xff];
            planes[7][off] = bitrev[ b        & 0xff];
            off++;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Same transpose, but 32 pixels at a time with LONG stores.
 *
 * c2p_rect() above stores 8 scattered single bytes per 8 pixels. Bitplanes live
 * in Chip RAM, where byte writes are the expensive part - the transpose itself
 * is not the bottleneck. Here each plane's four bytes are accumulated in a
 * register and written as one 32-bit store, cutting Chip RAM write cycles 4x.
 *
 * Requires x and w to be multiples of 32 (true for every rect we blit).
 * -------------------------------------------------------------------------*/
#define C2P_GROUP(a, b)                                       \
    do {                                                      \
        ULONG t;                                              \
        t = (((a) >> 4) ^ (b)) & 0x0f0f0f0fUL;                \
        (b) ^= t;  (a) ^= (t << 4);                           \
        t = ((a) ^ ((a) >> 18)) & 0x00003333UL; (a) ^= t ^ (t << 18); \
        t = ((b) ^ ((b) >> 18)) & 0x00003333UL; (b) ^= t ^ (t << 18); \
        t = ((a) ^ ((a) >>  9)) & 0x00550055UL; (a) ^= t ^ (t <<  9); \
        t = ((b) ^ ((b) >>  9)) & 0x00550055UL; (b) ^= t ^ (t <<  9); \
    } while (0)

static void c2p_rect32(const UBYTE *src, ULONG srcMod,
                       UBYTE **planes, ULONG bpr,
                       ULONG x, ULONG y, ULONG w, ULONG h)
{
    ULONG row, col, g;

    for (row = 0; row < h; row++) {
        const ULONG *s = (const ULONG *)(src + row * srcMod);
        ULONG        off = (y + row) * bpr + (x >> 3);

        for (col = 0; col < w; col += 32) {
            ULONG a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            ULONG a4 = 0, a5 = 0, a6 = 0, a7 = 0;

            for (g = 0; g < 4; g++) {
                ULONG a = *s++;
                ULONG b = *s++;
                C2P_GROUP(a, b);
                a0 = (a0 << 8) | bitrev[(a >> 24) & 0xff];
                a1 = (a1 << 8) | bitrev[(a >> 16) & 0xff];
                a2 = (a2 << 8) | bitrev[(a >>  8) & 0xff];
                a3 = (a3 << 8) | bitrev[ a        & 0xff];
                a4 = (a4 << 8) | bitrev[(b >> 24) & 0xff];
                a5 = (a5 << 8) | bitrev[(b >> 16) & 0xff];
                a6 = (a6 << 8) | bitrev[(b >>  8) & 0xff];
                a7 = (a7 << 8) | bitrev[ b        & 0xff];
            }
            *(ULONG *)(planes[0] + off) = a0;
            *(ULONG *)(planes[1] + off) = a1;
            *(ULONG *)(planes[2] + off) = a2;
            *(ULONG *)(planes[3] + off) = a3;
            *(ULONG *)(planes[4] + off) = a4;
            *(ULONG *)(planes[5] + off) = a5;
            *(ULONG *)(planes[6] + off) = a6;
            *(ULONG *)(planes[7] + off) = a7;
            off += 4;
        }
    }
}

/* Obviously-correct reference used once to validate c2p_rect(). */
static UBYTE ref_plane_byte(const UBYTE *src8, int plane)
{
    UBYTE m = 0;
    int i;
    for (i = 0; i < 8; i++)
        if ((src8[i] >> plane) & 1)
            m |= (UBYTE)(1 << (7 - i));
    return m;
}

int main(void)
{
    struct Screen  *scr    = NULL;
    struct BitMap  *tmpBM  = NULL;
    struct RastPort tmpRP;
    UBYTE  *chunky = NULL;
    UBYTE  *scratch = NULL;
    ULONG  *colTable = NULL;
    UBYTE  *planes[8];
    ULONG   bpr;
    BPTR    fh;
    LONG    got;
    ULONG   t0, dt;
    int     i, p, rc = 20, bad;

    /* rectangle sizes to profile: full frame, quarter, and a small GUI-ish area */
    static const struct { ULONG w, h, iters; const char *name; } tests[] = {
        { 640, 512,  10, "full frame 640x512" },
        { 320, 256,  40, "quarter     320x256" },
        { 160,  64, 200, "small rect  160x64 " },
        {  64,  32, 400, "tiny rect   64x32  " }
    };
    const int ntests = 4;

    for (i = 0; i < 256; i++) {
        UBYTE r = 0;
        for (p = 0; p < 8; p++)
            if ((i >> p) & 1) r |= (UBYTE)(1 << (7 - p));
        bitrev[i] = r;
    }

    chunky = (UBYTE *)AllocVec(SCR_W * SCR_H, MEMF_ANY);
    scratch = (UBYTE *)AllocVec(SCR_W * SCR_H, MEMF_ANY);
    if (chunky == NULL || scratch == NULL) { Printf("FAIL: AllocVec\n"); goto done; }

    fh = Open((STRPTR)"image.raw", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.raw\n"); goto done; }
    got = Read(fh, chunky, SCR_W * SCR_H);
    Close(fh);
    if (got != SCR_W * SCR_H) { Printf("FAIL: image.raw short read %ld\n", got); goto done; }

    fh = Open((STRPTR)"image.pal", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.pal\n"); goto done; }
    Read(fh, palette, 768);
    Close(fh);

    scr = OpenScreenTags(NULL,
                         SA_Width,     (ULONG)SCR_W,
                         SA_Height,    (ULONG)SCR_H,
                         SA_Depth,     (ULONG)SCR_DEPTH,
                         SA_Type,      (ULONG)CUSTOMSCREEN,
                         SA_Quiet,     (ULONG)TRUE,
                         SA_ShowTitle, (ULONG)FALSE,
                         SA_DisplayID, (ULONG)(PAL_MONITOR_ID | HIRESLACE_KEY),
                         TAG_END);
    if (scr == NULL) { Printf("FAIL: OpenScreenTags\n"); goto done; }

    colTable = (ULONG *)AllocVec((1 + 256 * 3 + 1) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    if (colTable == NULL) { Printf("FAIL: AllocVec colTable\n"); goto done; }
    colTable[0] = (256UL << 16) | 0UL;
    for (i = 0; i < 256; i++) {
        colTable[1 + i*3 + 0] = ((ULONG)palette[i*3 + 0]) * 0x01010101UL;
        colTable[1 + i*3 + 1] = ((ULONG)palette[i*3 + 1]) * 0x01010101UL;
        colTable[1 + i*3 + 2] = ((ULONG)palette[i*3 + 2]) * 0x01010101UL;
    }
    LoadRGB32(&scr->ViewPort, colTable);

    bpr = scr->RastPort.BitMap->BytesPerRow;
    for (p = 0; p < 8; p++) planes[p] = (UBYTE *)scr->RastPort.BitMap->Planes[p];
    Printf("screen %ld x %ld, BytesPerRow %ld, depth %ld\n",
           (LONG)scr->Width, (LONG)scr->Height, (LONG)bpr,
           (LONG)scr->RastPort.BitMap->Depth);

    /* ---- correctness gate: our c2p must match the reference exactly -------- */
    c2p_rect(chunky, SCR_W, planes, bpr, 0, 0, SCR_W, SCR_H);
    bad = 0;
    for (i = 0; i < 64 && !bad; i++) {              /* sample 64 groups */
        ULONG gx = (ULONG)(i * 8) % SCR_W;
        ULONG gy = (ULONG)(i * 7) % SCR_H;
        for (p = 0; p < 8; p++) {
            UBYTE want = ref_plane_byte(chunky + gy * SCR_W + gx, p);
            UBYTE have = planes[p][gy * bpr + (gx >> 3)];
            if (want != have) {
                Printf("FAIL: c2p mismatch at x=%ld y=%ld plane %ld: want %ld have %ld\n",
                       (LONG)gx, (LONG)gy, (LONG)p, (LONG)want, (LONG)have);
                bad = 1;
                break;
            }
        }
    }
    if (bad) goto done;
    Printf("OK: our c2p matches the reference (image is on screen now)\n\n");
    Delay(50 * 3);

    /* ---- benchmark A: system WritePixelArray8 ----------------------------- */
    tmpBM = AllocBitMap(SCR_W, 1, SCR_DEPTH, BMF_CLEAR, NULL);
    if (tmpBM == NULL) { Printf("FAIL: AllocBitMap temp\n"); goto done; }
    InitRastPort(&tmpRP);
    tmpRP.BitMap = tmpBM;

    Printf("=== WritePixelArray8 (graphics.library) ===\n");
    for (i = 0; i < ntests; i++) {
        ULONG w = tests[i].w, h = tests[i].h, n = tests[i].iters, k;
        ULONG ms, fps10;
        /* source must be a contiguous w x h array */
        for (k = 0; k < h; k++)
            CopyMem(chunky + k * SCR_W, scratch + k * w, w);

        t0 = now_ticks();
        for (k = 0; k < n; k++)
            WritePixelArray8(&scr->RastPort, 0, 0, w - 1, h - 1, scratch, &tmpRP);
        dt = now_ticks() - t0;

        ms = (dt * 20UL) / n;                       /* 1 tick = 20 ms */
        fps10 = ms ? (10000UL / ms) : 9999;
        Printf("%s : %4ld ms/blit  -> %ld.%ld full-rect blits/s\n",
               (LONG)tests[i].name, (LONG)ms, (LONG)(fps10 / 10), (LONG)(fps10 % 10));
    }

    /* ---- benchmark B: our own c2p ----------------------------------------- */
    Printf("\n=== our c2p (68k C, 8x8 bit transpose) ===\n");
    for (i = 0; i < ntests; i++) {
        ULONG w = tests[i].w, h = tests[i].h, n = tests[i].iters * 4, k;
        ULONG ms10, fps10;

        t0 = now_ticks();
        for (k = 0; k < n; k++)
            c2p_rect(chunky, SCR_W, planes, bpr, 0, 0, w, h);
        dt = now_ticks() - t0;

        ms10 = (dt * 200UL) / n;                    /* tenths of a ms */
        fps10 = ms10 ? (100000UL / ms10) : 9999;
        Printf("%s : %4ld.%ld ms/blit -> %ld.%ld full-rect blits/s\n",
               (LONG)tests[i].name, (LONG)(ms10 / 10), (LONG)(ms10 % 10),
               (LONG)(fps10 / 10), (LONG)(fps10 % 10));
    }

    /* ---- benchmark C: our c2p with 32-bit stores --------------------------- */
    /* validate it against the same reference before trusting its timings */
    memset(planes[0], 0, bpr * SCR_H);
    c2p_rect32(chunky, SCR_W, planes, bpr, 0, 0, SCR_W, SCR_H);
    bad = 0;
    for (i = 0; i < 64 && !bad; i++) {
        ULONG gx = (ULONG)(i * 8) % SCR_W;
        ULONG gy = (ULONG)(i * 7) % SCR_H;
        for (p = 0; p < 8; p++) {
            UBYTE want = ref_plane_byte(chunky + gy * SCR_W + gx, p);
            UBYTE have = planes[p][gy * bpr + (gx >> 3)];
            if (want != have) {
                Printf("FAIL: c2p32 mismatch x=%ld y=%ld plane %ld\n",
                       (LONG)gx, (LONG)gy, (LONG)p);
                bad = 1;
                break;
            }
        }
    }
    if (bad) goto done;

    Printf("\n=== our c2p, 32-bit stores (4x fewer Chip RAM writes) ===\n");
    for (i = 0; i < ntests; i++) {
        ULONG w = tests[i].w, h = tests[i].h, n = tests[i].iters * 4, k;
        ULONG ms10, fps10;

        t0 = now_ticks();
        for (k = 0; k < n; k++)
            c2p_rect32(chunky, SCR_W, planes, bpr, 0, 0, w, h);
        dt = now_ticks() - t0;

        ms10 = (dt * 200UL) / n;
        fps10 = ms10 ? (100000UL / ms10) : 9999;
        Printf("%s : %4ld.%ld ms/blit -> %ld.%ld full-rect blits/s\n",
               (LONG)tests[i].name, (LONG)(ms10 / 10), (LONG)(ms10 % 10),
               (LONG)(fps10 / 10), (LONG)(fps10 % 10));
    }

    Printf("\nDONE\n");
    Delay(50 * 5);
    rc = 0;

done:
    if (tmpBM)    FreeBitMap(tmpBM);
    if (scr)      CloseScreen(scr);
    if (colTable) FreeVec(colTable);
    if (scratch)  FreeVec(scratch);
    if (chunky)   FreeVec(chunky);
    if (rc != 0)  Delay(50 * 8);
    return rc;
}
