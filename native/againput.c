/* Step 3 of the native Amiga display engine: input + the render strategy that
 * Step 2's measurements forced on us.
 *
 * Proves three things that the OpenTTD video driver needs, on plain AGA:
 *   1. IDCMP input works on our own screen - mouse position, buttons, keys.
 *   2. Dirty-rect blitting is fast enough to move a cursor interactively
 *      (Step 2: full frame = 782 ms, but 32x32 = ~1 ms).
 *   3. ScrollRaster() + refilling only the exposed strip is what makes viewport
 *      panning affordable - the alternative is a 782 ms full repaint.
 *
 * Controls: move mouse / left button = mark, arrow keys = scroll, ESC = quit.
 * Everything measured is written to stdout, so run it with >file to get a log.
 *
 * Build (Windows toolchain):
 *   m68k-amigaos-gcc -m68040 -O2 -noixemul -o againput againput.c
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

#define SCR_W   640
#define SCR_H   512
#define SCR_DEPTH 8
#define CUR_SZ  32              /* cursor dirty rect, multiple of 16 */
#define SCROLL  16              /* pixels per scroll step */
#define RUN_SEC 75              /* auto-quit so an unattended run always ends */

unsigned long __stack = 1024 * 1024;

static UBYTE palette[768];
static UBYTE master[SCR_W * SCR_H];     /* the "OpenTTD framebuffer" */
static UBYTE work[CUR_SZ * CUR_SZ];     /* scratch for one dirty rect */
static UBYTE strip[SCR_W * SCROLL];     /* scratch for one exposed strip */

static struct RastPort tmpRP;           /* WritePixelArray8 needs this */

static ULONG now_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

/* Blit a w x h rect of the master framebuffer to (x,y). This is exactly the
 * primitive the OpenTTD driver will expose as "make this dirty rect visible". */
static void blit_rect(struct RastPort *rp, LONG x, LONG y, LONG w, LONG h)
{
    UBYTE *dst = work;
    LONG   r;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    w &= ~1L;                            /* WritePixelArray8 wants even width */
    if (w <= 0) return;

    for (r = 0; r < h; r++)
        CopyMem(master + (y + r) * SCR_W + x, dst + r * w, (ULONG)w);

    WritePixelArray8(rp, (UWORD)x, (UWORD)y,
                     (UWORD)(x + w - 1), (UWORD)(y + h - 1), dst, &tmpRP);
}

/* Stamp a hollow box into the master buffer - stands in for "the game drew
 * something here", so we can see dirty-rect updates actually landing. */
static void stamp_mark(LONG cx, LONG cy, UBYTE pen)
{
    LONG x0 = cx - 6, y0 = cy - 6, i;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x0 > SCR_W - 13) x0 = SCR_W - 13;
    if (y0 > SCR_H - 13) y0 = SCR_H - 13;
    for (i = 0; i < 13; i++) {
        master[y0 * SCR_W + x0 + i]           = pen;
        master[(y0 + 12) * SCR_W + x0 + i]    = pen;
        master[(y0 + i) * SCR_W + x0]         = pen;
        master[(y0 + i) * SCR_W + x0 + 12]    = pen;
    }
}

int main(void)
{
    struct Screen *scr = NULL;
    struct Window *win = NULL;
    struct BitMap *tmpBM = NULL;
    struct IntuiMessage *msg;
    ULONG  *colTable = NULL;
    BPTR    fh;
    LONG    got, i;
    LONG    mx = SCR_W / 2, my = SCR_H / 2, ox, oy;
    ULONG   t0, tEnd, dt;
    ULONG   nMoves = 0, nKeys = 0, nClicks = 0, nBlits = 0;
    ULONG   scrollTicks = 0, nScrolls = 0, nTicks = 0, lastKey = 0;
    int     rc = 20, running = 1;

    fh = Open((STRPTR)"image.raw", MODE_OLDFILE);
    if (fh == 0) { Printf("FAIL: cannot open image.raw\n"); goto done; }
    got = Read(fh, master, SCR_W * SCR_H);
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

    tmpBM = AllocBitMap(SCR_W, 1, SCR_DEPTH, BMF_CLEAR, NULL);
    if (tmpBM == NULL) { Printf("FAIL: AllocBitMap\n"); goto done; }
    InitRastPort(&tmpRP);
    tmpRP.BitMap = tmpBM;

    /* A backdrop, borderless window is how you get IDCMP on your own screen
     * without giving up the full display area. RMBTRAP keeps the right button
     * for the game instead of Intuition menus. */
    win = OpenWindowTags(NULL,
                         WA_CustomScreen, (ULONG)scr,
                         WA_Left,   0UL, WA_Top, 0UL,
                         WA_Width,  (ULONG)SCR_W, WA_Height, (ULONG)SCR_H,
                         WA_Flags,  (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS |
                                            WFLG_ACTIVATE | WFLG_REPORTMOUSE |
                                            WFLG_RMBTRAP | WFLG_NOCAREREFRESH),
                         /* INTUITICKS (~10 Hz) is the heartbeat: without it the
                          * loop blocks in WaitPort forever when no input ever
                          * arrives, and an unattended run can never time out. */
                         WA_IDCMP,  (ULONG)(IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
                                            IDCMP_RAWKEY | IDCMP_INTUITICKS),
                         TAG_END);
    if (win == NULL) { Printf("FAIL: OpenWindowTags\n"); goto done; }

    /* one full repaint to get started - the only one we can afford */
    t0 = now_ticks();
    {
        UBYTE *tmp = (UBYTE *)AllocVec(SCR_W * SCR_H, MEMF_ANY);
        if (tmp) {
            CopyMem(master, tmp, SCR_W * SCR_H);
            WritePixelArray8(win->RPort, 0, 0, SCR_W - 1, SCR_H - 1, tmp, &tmpRP);
            FreeVec(tmp);
        }
    }
    Printf("full repaint: %ld ms\n", (LONG)((now_ticks() - t0) * 20));
    Printf("ready - move mouse, click, arrows scroll, ESC quits (auto-quit %ld s)\n",
           (LONG)RUN_SEC);

    t0    = now_ticks();
    tEnd  = t0 + RUN_SEC * 50UL;
    ox = mx; oy = my;

    while (running) {
        WaitPort(win->UserPort);
        while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
            ULONG cls  = msg->Class;
            UWORD code = msg->Code;
            WORD  nx   = msg->MouseX;
            WORD  ny   = msg->MouseY;
            ReplyMsg((struct Message *)msg);

            switch (cls) {
            case IDCMP_MOUSEMOVE:
                mx = nx; my = ny;
                nMoves++;
                /* restore what the cursor covered, then draw it at the new spot:
                 * two small blits instead of one 782 ms frame */
                blit_rect(win->RPort, ox - CUR_SZ/2, oy - CUR_SZ/2, CUR_SZ, CUR_SZ);
                stamp_mark(mx, my, 255);
                blit_rect(win->RPort, mx - CUR_SZ/2, my - CUR_SZ/2, CUR_SZ, CUR_SZ);
                /* un-stamp so the mark does not persist in the framebuffer */
                stamp_mark(mx, my, 0);
                nBlits += 2;
                ox = mx; oy = my;
                break;

            case IDCMP_MOUSEBUTTONS:
                if (code == SELECTDOWN) {
                    nClicks++;
                    stamp_mark(mx, my, 1);          /* permanent mark */
                    blit_rect(win->RPort, mx - CUR_SZ/2, my - CUR_SZ/2, CUR_SZ, CUR_SZ);
                    nBlits++;
                }
                break;

            case IDCMP_INTUITICKS:
                nTicks++;
                /* periodic counter dump: lets the host correlate what it
                 * injected with what actually reached IDCMP */
                if ((nTicks % 20) == 0)
                    Printf("t+%ld s: moves=%ld clicks=%ld keys=%ld lastkey=$%lx pos=%ld,%ld\n",
                           (LONG)((now_ticks() - t0) / 50), (LONG)nMoves,
                           (LONG)nClicks, (LONG)nKeys, (LONG)lastKey,
                           (LONG)mx, (LONG)my);
                break;

            case IDCMP_RAWKEY:
                lastKey = code;
                if (code == 0x45) { running = 0; break; }   /* ESC */
                nKeys++;
                if (code == 0x4C || code == 0x4D) {         /* up / down */
                    LONG dy = (code == 0x4C) ? -SCROLL : SCROLL;
                    ULONG s0 = now_ticks();
                    ScrollRaster(win->RPort, 0, dy, 0, 0, SCR_W - 1, SCR_H - 1);
                    /* only the newly exposed strip needs chunky->planar */
                    {
                        LONG sy = (dy < 0) ? (SCR_H - SCROLL) : 0;
                        LONG r;
                        for (r = 0; r < SCROLL; r++)
                            CopyMem(master + ((sy + r) % SCR_H) * SCR_W,
                                    strip + r * SCR_W, SCR_W);
                        WritePixelArray8(win->RPort, 0, (UWORD)sy,
                                         SCR_W - 1, (UWORD)(sy + SCROLL - 1),
                                         strip, &tmpRP);
                    }
                    scrollTicks += now_ticks() - s0;
                    nScrolls++;
                }
                break;
            }
        }
        if (now_ticks() >= tEnd) running = 0;
    }

    dt = now_ticks() - t0;
    Printf("\n--- input session over %ld.%ld s ---\n",
           (LONG)(dt / 50), (LONG)((dt % 50) * 2 / 10));
    Printf("mouse moves : %ld\n", (LONG)nMoves);
    Printf("clicks      : %ld\n", (LONG)nClicks);
    Printf("keys        : %ld\n", (LONG)nKeys);
    Printf("dirty blits : %ld (%ld x %ld each)\n", (LONG)nBlits, (LONG)CUR_SZ, (LONG)CUR_SZ);
    if (dt)
        Printf("blit rate   : %ld dirty rects/s sustained\n", (LONG)(nBlits * 50 / dt));
    if (nScrolls)
        Printf("scroll step : %ld ms avg (ScrollRaster + %ld px strip), %ld steps\n",
               (LONG)(scrollTicks * 20 / nScrolls), (LONG)SCROLL, (LONG)nScrolls);
    else
        Printf("scroll step : not exercised (no arrow keys received)\n");
    Printf("DONE\n");
    rc = 0;

done:
    if (win)      CloseWindow(win);
    if (tmpBM)    FreeBitMap(tmpBM);
    if (scr)      CloseScreen(scr);
    if (colTable) FreeVec(colTable);
    if (rc != 0)  Delay(50 * 8);
    return rc;
}
