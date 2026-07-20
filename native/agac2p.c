/* Kalms c2p vs graphics.library WritePixelArray8, on a calibrated 68040.
 *
 * Run this on winuae/otd-aga-040-25.uae (cpu_throttle=-800.0, SysInfo reads
 * 0.94 against an A4000/040/25) so the milliseconds printed are real-hardware
 * milliseconds. WinUAE writes that slider in TENTHS of a percent: -800.0 = -80%.
 *
 * c2p_rect needs all eight bitplanes contiguous and equally spaced, which
 * AllocBitMap does not guarantee, so we allocate one Chip RAM block, build the
 * BitMap by hand and hand it to Intuition with SA_BitMap.
 *
 * Build:
 *   vasmm68k_mot -Fhunk -m68040 -no-opt -o c2p_glue.o c2p_glue.s
 *   m68k-amigaos-gcc -m68040 -O2 -noixemul -o agac2p agac2p.c c2p_glue.o
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
#define BPR   (SCR_W / 8)
#define PLANESIZE ((ULONG)BPR * SCR_H)

unsigned long __stack = 1024 * 1024;

struct C2PArgs {
    UWORD x, y, w, h;
    UWORD cmod, bmod;
    ULONG bplsize;
    APTR  chunky;
    APTR  bpl;
};

extern void c2p_rect_asm(struct C2PArgs *a);

static UBYTE palette[768];
static UBYTE master[SCR_W * SCR_H];

static ULONG now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* reference bit for validating the assembly routine */
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
    struct Screen  *scr = NULL;
    struct BitMap   bm;
    struct BitMap  *tmpBM = NULL;
    struct RastPort tmpRP;
    UBYTE  *chip = NULL;
    UBYTE  *scratch = NULL;
    ULONG  *colTable = NULL;
    struct C2PArgs args;
    BPTR    fh;
    LONG    got;
    ULONG   t0, dt;
    int     i, p, rc = 20, bad;

    static const struct { ULONG w, h, iters; const char *name; } tests[] = {
        { 640, 512,  10, "full frame 640x512" },
        { 320, 256,  40, "quarter     320x256" },
        { 160,  64, 200, "small rect  160x64 " },
        {  64,  32, 400, "tiny rect   64x32  " }
    };
    const int ntests = 4;

    fh = Open((STRPTR)"image.raw", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.raw\n"); goto done; }
    got = Read(fh, master, SCR_W * SCR_H);
    Close(fh);
    if (got != SCR_W * SCR_H) { Printf("FAIL: short read %ld\n", got); goto done; }

    fh = Open((STRPTR)"image.pal", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.pal\n"); goto done; }
    Read(fh, palette, 768);
    Close(fh);

    scratch = (UBYTE *)AllocVec(SCR_W * SCR_H, MEMF_ANY);
    if (scratch == NULL) { Printf("FAIL: AllocVec scratch\n"); goto done; }

    /* one contiguous Chip RAM block for all eight planes */
    chip = (UBYTE *)AllocMem(PLANESIZE * SCR_DEPTH, MEMF_CHIP | MEMF_CLEAR);
    if (chip == NULL) { Printf("FAIL: AllocMem %ld bytes of Chip RAM\n",
                               (LONG)(PLANESIZE * SCR_DEPTH)); goto done; }
    InitBitMap(&bm, SCR_DEPTH, SCR_W, SCR_H);
    for (i = 0; i < SCR_DEPTH; i++)
        bm.Planes[i] = (PLANEPTR)(chip + (ULONG)i * PLANESIZE);

    scr = OpenScreenTags(NULL,
                         SA_BitMap,    (ULONG)&bm,
                         SA_Width,     (ULONG)SCR_W,
                         SA_Height,    (ULONG)SCR_H,
                         SA_Depth,     (ULONG)SCR_DEPTH,
                         SA_Type,      (ULONG)CUSTOMSCREEN,
                         SA_Quiet,     (ULONG)TRUE,
                         SA_ShowTitle, (ULONG)FALSE,
                         SA_DisplayID, (ULONG)(PAL_MONITOR_ID | HIRESLACE_KEY),
                         TAG_END);
    if (scr == NULL) { Printf("FAIL: OpenScreenTags with custom BitMap\n"); goto done; }

    colTable = (ULONG *)AllocVec((1 + 256 * 3 + 1) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    if (colTable == NULL) { Printf("FAIL: AllocVec colTable\n"); goto done; }
    colTable[0] = (256UL << 16) | 0UL;
    for (i = 0; i < 256; i++) {
        colTable[1 + i*3 + 0] = ((ULONG)palette[i*3 + 0]) * 0x01010101UL;
        colTable[1 + i*3 + 1] = ((ULONG)palette[i*3 + 1]) * 0x01010101UL;
        colTable[1 + i*3 + 2] = ((ULONG)palette[i*3 + 2]) * 0x01010101UL;
    }
    LoadRGB32(&scr->ViewPort, colTable);

    /* ---- correctness gate for the assembly routine ------------------------ */
    args.x = 0; args.y = 0; args.w = SCR_W; args.h = SCR_H;
    args.cmod = SCR_W; args.bmod = BPR; args.bplsize = PLANESIZE;
    args.chunky = master; args.bpl = chip;
    c2p_rect_asm(&args);

    bad = 0;
    for (i = 0; i < 64 && !bad; i++) {
        ULONG gx = (ULONG)(i * 8) % SCR_W;
        ULONG gy = (ULONG)(i * 7) % SCR_H;
        for (p = 0; p < SCR_DEPTH; p++) {
            UBYTE want = ref_plane_byte(master + gy * SCR_W + gx, p);
            UBYTE have = chip[(ULONG)p * PLANESIZE + gy * BPR + (gx >> 3)];
            if (want != have) {
                Printf("FAIL: Kalms c2p mismatch x=%ld y=%ld plane %ld want %ld have %ld\n",
                       (LONG)gx, (LONG)gy, (LONG)p, (LONG)want, (LONG)have);
                bad = 1;
                break;
            }
        }
    }
    if (bad) goto done;
    Printf("OK: Kalms c2p_rect is bit-exact (image is on screen)\n\n");
    Delay(50 * 3);

    tmpBM = AllocBitMap(SCR_W, 1, SCR_DEPTH, BMF_CLEAR, NULL);
    if (tmpBM == NULL) { Printf("FAIL: AllocBitMap temp\n"); goto done; }
    InitRastPort(&tmpRP);
    tmpRP.BitMap = tmpBM;

    Printf("=== graphics.library WritePixelArray8 ===\n");
    for (i = 0; i < ntests; i++) {
        ULONG w = tests[i].w, h = tests[i].h, n = tests[i].iters, k;
        ULONG ms10, fps10;
        for (k = 0; k < h; k++)
            CopyMem(master + k * SCR_W, scratch + k * w, w);
        t0 = now_ticks();
        for (k = 0; k < n; k++)
            WritePixelArray8(&scr->RastPort, 0, 0, w - 1, h - 1, scratch, &tmpRP);
        dt = now_ticks() - t0;
        ms10 = (dt * 200UL) / n;
        fps10 = ms10 ? (100000UL / ms10) : 9999;
        Printf("%s : %4ld.%ld ms -> %ld.%ld /s\n", (LONG)tests[i].name,
               (LONG)(ms10 / 10), (LONG)(ms10 % 10), (LONG)(fps10 / 10), (LONG)(fps10 % 10));
    }

    Printf("\n=== Kalms c2p_rect (68040 asm, dirty rect) ===\n");
    for (i = 0; i < ntests; i++) {
        ULONG w = tests[i].w, h = tests[i].h, n = tests[i].iters * 4, k;
        ULONG ms10, fps10;
        args.x = 0; args.y = 0; args.w = (UWORD)w; args.h = (UWORD)h;
        args.cmod = SCR_W; args.bmod = BPR; args.bplsize = PLANESIZE;
        args.chunky = master; args.bpl = chip;
        t0 = now_ticks();
        for (k = 0; k < n; k++)
            c2p_rect_asm(&args);
        dt = now_ticks() - t0;
        ms10 = (dt * 200UL) / n;
        fps10 = ms10 ? (100000UL / ms10) : 9999;
        Printf("%s : %4ld.%ld ms -> %ld.%ld /s\n", (LONG)tests[i].name,
               (LONG)(ms10 / 10), (LONG)(ms10 % 10), (LONG)(fps10 / 10), (LONG)(fps10 % 10));
    }

    Printf("\nDONE\n");
    Delay(50 * 4);
    rc = 0;

done:
    if (tmpBM)    FreeBitMap(tmpBM);
    if (scr)      CloseScreen(scr);
    if (colTable) FreeVec(colTable);
    if (scratch)  FreeVec(scratch);
    if (chip)     FreeMem(chip, PLANESIZE * SCR_DEPTH);
    if (rc != 0)  Delay(50 * 8);
    return rc;
}
