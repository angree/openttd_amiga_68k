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

void OldAI_Log(const char *s)
{
	if (g_oldai_log == 0) {
		g_oldai_log = Open((STRPTR)"Work:oldai.log", MODE_NEWFILE);
		if (g_oldai_log == 0) return;
	}
	Write(g_oldai_log, (APTR)s, (LONG)strlen(s));
	Write(g_oldai_log, (APTR)"\n", 1);
}
