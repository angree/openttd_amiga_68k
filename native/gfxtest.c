/* Milestone 1 of the native Amiga display engine for OpenTTD.
 *
 * Goal: open a 640x512, 256-colour screen and display one real image from a
 * chunky 8bpp buffer + palette (exactly the data shape OpenTTD produces in
 * _screen.dst_ptr / _cur_palette).
 *
 * Deliberately uses graphics.library WritePixelArray8() for the chunky->planar
 * step: it works on plain AGA with no custom c2p code. It is slow; a fast c2p
 * replaces it once the picture is proven on screen. Correctness first.
 *
 * Build (Windows toolchain):
 *   m68k-amigaos-gcc -m68040 -O2 -noixemul -o gfxtest gfxtest.c
 * Run on the Amiga from the directory holding image.raw / image.pal.
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

#define SCR_W 640
#define SCR_H 512
#define SCR_DEPTH 8

/* OpenTTD needs a big stack; the CLI default (~4KB) is nowhere near enough. */
unsigned long __stack = 1024 * 1024;

static UBYTE palette[768];

int main(void)
{
    struct Screen  *scr    = NULL;
    struct BitMap  *tmpBM  = NULL;
    struct RastPort tmpRP;
    UBYTE  *chunky = NULL;
    ULONG  *colTable = NULL;
    BPTR    fh;
    LONG    got;
    int     i, rc = 20;

    /* ---- load the chunky image ------------------------------------------ */
    chunky = (UBYTE *)AllocVec(SCR_W * SCR_H, MEMF_ANY);
    if (chunky == NULL) { Printf("FAIL: AllocVec %ld bytes\n", (LONG)(SCR_W*SCR_H)); goto done; }

    fh = Open((STRPTR)"image.raw", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.raw (run me from its directory)\n"); goto done; }
    got = Read(fh, chunky, SCR_W * SCR_H);
    Close(fh);
    if (got != SCR_W * SCR_H) { Printf("FAIL: image.raw short read: %ld\n", got); goto done; }

    fh = Open((STRPTR)"image.pal", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.pal\n"); goto done; }
    got = Read(fh, palette, 768);
    Close(fh);
    if (got != 768) { Printf("FAIL: image.pal short read: %ld\n", got); goto done; }
    Printf("OK: loaded image.raw + image.pal\n");

    /* ---- open a 640x512 8-bitplane screen -------------------------------- */
    scr = OpenScreenTags(NULL,
                         SA_Width,     (ULONG)SCR_W,
                         SA_Height,    (ULONG)SCR_H,
                         SA_Depth,     (ULONG)SCR_DEPTH,
                         SA_Type,      (ULONG)CUSTOMSCREEN,
                         SA_Quiet,     (ULONG)TRUE,
                         SA_ShowTitle, (ULONG)FALSE,
                         SA_DisplayID, (ULONG)(PAL_MONITOR_ID | HIRESLACE_KEY),
                         TAG_END);
    if (scr == NULL) {
        /* fall back: let the system choose a mode for 640x512x8 */
        Printf("note: PAL HiRes-Laced failed, retrying with system-chosen mode\n");
        scr = OpenScreenTags(NULL,
                             SA_Width,     (ULONG)SCR_W,
                             SA_Height,    (ULONG)SCR_H,
                             SA_Depth,     (ULONG)SCR_DEPTH,
                             SA_Type,      (ULONG)CUSTOMSCREEN,
                             SA_Quiet,     (ULONG)TRUE,
                             SA_ShowTitle, (ULONG)FALSE,
                             TAG_END);
    }
    if (scr == NULL) { Printf("FAIL: OpenScreenTags 640x512x8\n"); goto done; }
    Printf("OK: screen open (%ld x %ld, depth %ld)\n",
           (LONG)scr->Width, (LONG)scr->Height, (LONG)SCR_DEPTH);

    /* ---- set the 256-colour palette -------------------------------------- */
    /* LoadRGB32 table: header (count<<16 | firstcolour), then 3 x 32-bit per pen, then 0 */
    colTable = (ULONG *)AllocVec((1 + 256 * 3 + 1) * sizeof(ULONG), MEMF_ANY | MEMF_CLEAR);
    if (colTable == NULL) { Printf("FAIL: AllocVec colTable\n"); goto done; }
    colTable[0] = (256UL << 16) | 0UL;
    for (i = 0; i < 256; i++) {
        colTable[1 + i*3 + 0] = ((ULONG)palette[i*3 + 0]) * 0x01010101UL;
        colTable[1 + i*3 + 1] = ((ULONG)palette[i*3 + 1]) * 0x01010101UL;
        colTable[1 + i*3 + 2] = ((ULONG)palette[i*3 + 2]) * 0x01010101UL;
    }
    colTable[1 + 256*3] = 0UL;
    LoadRGB32(&scr->ViewPort, colTable);
    Printf("OK: palette loaded (256 pens)\n");

    /* ---- chunky -> screen ------------------------------------------------- */
    /* WritePixelArray8 needs a temporary RastPort whose BitMap is at least as
     * wide as the blitted area and one row high, same depth as the target.    */
    tmpBM = AllocBitMap(SCR_W, 1, SCR_DEPTH, BMF_CLEAR, NULL);
    if (tmpBM == NULL) { Printf("FAIL: AllocBitMap temp\n"); goto done; }
    InitRastPort(&tmpRP);
    tmpRP.BitMap = tmpBM;

    Printf("blitting %ld x %ld chunky pixels...\n", (LONG)SCR_W, (LONG)SCR_H);
    WritePixelArray8(&scr->RastPort, 0, 0, SCR_W - 1, SCR_H - 1, chunky, &tmpRP);
    Printf("OK: image on screen - holding 20 seconds\n");

    Delay(50 * 20);   /* 20 seconds at 50 ticks/second */
    rc = 0;

done:
    if (tmpBM)   FreeBitMap(tmpBM);
    if (scr)     CloseScreen(scr);
    if (colTable) FreeVec(colTable);
    if (chunky)  FreeVec(chunky);
    if (rc != 0) Delay(50 * 8);   /* keep failure text readable on screen */
    return rc;
}
