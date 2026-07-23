/* Dedicated dos.library logger for the native C++ AI, in its OWN C file.
 *
 * It must be separate from oldai.cpp: OpenTTD's headers do `#define Point
 * OTTD_Point`, which collides with the struct Point that the AmigaOS <proto/*>
 * headers pull in. Keeping the Amiga side in a plain C translation unit avoids
 * the clash, exactly as amiga_audio.c does for the sound driver.
 *
 * The log survives the shell's ">NIL:" because it is written straight through
 * dos.library, not stdout.
 */
#include <proto/dos.h>
#include <string.h>

static BPTR g_oldai_log = 0;
static int  g_oldai_log_state = 0;   /* 0 = undecided, 1 = enabled, -1 = disabled */

/* OPT-IN logging. The AI writes a line on nearly every decision, straight to disk
 * via dos.library - if that ran unconditionally in a shipped build it would thrash
 * the user's hard drive continuously (which is exactly what was reported). So the
 * log is OFF unless the user has explicitly created the marker file
 *   Work:oldai.log.enable
 * A normal player never has that file, so the shipped build does ZERO log writes.
 * To debug, create an empty "Work:oldai.log.enable" and the log turns on next run.
 * The decision is made once (first call) and cached, so the check costs nothing. */
void OldAI_Log(const char *s)
{
	if (g_oldai_log_state == 0) {
		BPTR lock = Lock((STRPTR)"Work:oldai.log.enable", ACCESS_READ);
		if (lock == 0) { g_oldai_log_state = -1; return; }
		UnLock(lock);
		g_oldai_log_state = 1;
	}
	if (g_oldai_log_state < 0) return;

	if (g_oldai_log == 0) {
		g_oldai_log = Open((STRPTR)"Work:oldai.log", MODE_NEWFILE);
		if (g_oldai_log == 0) { g_oldai_log_state = -1; return; }
	}
	Write(g_oldai_log, (APTR)s, (LONG)strlen(s));
	Write(g_oldai_log, (APTR)"\n", 1);
}
