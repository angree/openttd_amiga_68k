/* AmigaOS directory scanner for the native music driver.
 *
 * This deliberately stays plain C.  AmigaOS <proto/*> headers collide with
 * an OpenTTD C++ Point declaration, so no OpenTTD header may be included
 * here and no AmigaOS header may leak through amiga_mscan.h.
 */

#include <proto/dos.h>
#include <proto/intuition.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "amiga_mscan.h"

static int AsciiLower(int c)
{
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	return c;
}

/* Does name end with ext (".wav", ".mid", ".gm"), ignoring ASCII case?
 *
 * The extension is a parameter because the same scanner now serves three music
 * sources: sampled WAV out of music/, OpenMSX .mid out of gm/, and the player's
 * own original .gm out of gm/. Only the suffix differs. */
static int HasExtension(const char *name, size_t len, const char *ext)
{
	size_t ext_len = strlen(ext);
	size_t i;

	if (len <= ext_len) return 0;   /* need at least one basename character */
	for (i = 0; i < ext_len; i++) {
		if (AsciiLower((unsigned char)name[len - ext_len + i]) !=
				AsciiLower((unsigned char)ext[i])) {
			return 0;
		}
	}
	return 1;
}

int AmigaMusic_ScanDir(const char *dir,
		char paths[][AMIGA_MUSIC_PATH_MAX],
		char names[][AMIGA_MUSIC_NAME_MAX], int max)
{
	return AmigaMusic_ScanDirExt(dir, ".wav", paths, names, max);
}

int AmigaMusic_ScanDirExt(const char *dir, const char *ext,
		char paths[][AMIGA_MUSIC_PATH_MAX],
		char names[][AMIGA_MUSIC_NAME_MAX], int max)
{
	BPTR lock;
	struct FileInfoBlock *fib;
	size_t dir_len;
	int add_slash;
	int count;

	size_t ext_len;

	if (dir == NULL || ext == NULL || paths == NULL || names == NULL || max <= 0) return 0;
	ext_len = strlen(ext);
	if (ext_len == 0 || ext_len >= AMIGA_MUSIC_NAME_MAX) return 0;

	dir_len = strlen(dir);
	if (dir_len == 0 || dir_len >= AMIGA_MUSIC_PATH_MAX) return 0;
	add_slash = dir[dir_len - 1] != '/' && dir[dir_len - 1] != ':';

	lock = Lock((CONST_STRPTR)dir, ACCESS_READ);
	if (lock == (BPTR)0) return 0;

	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib == NULL) {
		UnLock(lock);
		return 0;
	}

	count = 0;
	if (Examine(lock, fib)) {
		while (count < max && ExNext(lock, fib)) {
			const char *filename;
			size_t file_len;
			size_t base_len;
			size_t name_copy;
			size_t pos;

			filename = (const char *)fib->fib_FileName;
			file_len = strlen(filename);

			/* AmigaDOS reports directories with a non-negative entry type.
			 * Dotfiles and entries with the wrong suffix are not part of the set. */
			if (fib->fib_DirEntryType >= 0) continue;
			if (filename[0] == '.') continue;
			if (!HasExtension(filename, file_len, ext)) continue;

			if (dir_len + (add_slash ? 1 : 0) + file_len + 1 >
					AMIGA_MUSIC_PATH_MAX) {
				continue;
			}

			memcpy(paths[count], dir, dir_len);
			pos = dir_len;
			if (add_slash) paths[count][pos++] = '/';
			memcpy(paths[count] + pos, filename, file_len + 1);

			base_len = file_len - ext_len;
			name_copy = base_len;
			if (name_copy >= AMIGA_MUSIC_NAME_MAX) {
				name_copy = AMIGA_MUSIC_NAME_MAX - 1;
			}
			memcpy(names[count], filename, name_copy);
			names[count][name_copy] = '\0';
			count++;
		}
	}

	FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	return count;
}

/* Resolve PROGDIR: to a real AmigaDOS path, e.g. "Work:Games/OpenTTD".
 *
 * It lives here because this is the one plain-C file in the port that may
 * include <proto/dos.h> - the C++ side cannot, the OTTD_Point collision
 * documented in amiga_m.cpp is why. The caller is fileio.cpp, which needs it
 * to find lang/, data/ and openttd.cfg no matter what the current directory
 * is when the game is started.
 *
 * Returns NULL if PROGDIR: cannot be resolved, so the caller can fall back.
 */
/* Silence the game when it is started from its Workbench icon.
 *
 * A program launched from Workbench has no console attached. The moment
 * anything is written to stdout the C library opens one for it, and that is
 * the window that pops up over the game when it is started from the icon -
 * full of debug output nobody asked for.
 *
 * Started from a Shell the output IS wanted: it goes wherever the user pointed
 * it, which is how Work:run captures it. So the test is simply whether we have
 * a CLI. Cli() returns NULL only for a Workbench launch. */
void AmigaSilenceIfWorkbench(void)
{
	if (Cli() != 0) return;      /* a Shell started us - leave the output alone */

	freopen("NIL:", "w", stdout);
	freopen("NIL:", "w", stderr);
}

const char *AmigaProgDirPath(void)
{
	static char buf[AMIGA_MUSIC_PATH_MAX];
	static int resolved = 0;

	if (!resolved) {
		BPTR lock = Lock("PROGDIR:", ACCESS_READ);
		buf[0] = '\0';
		if (lock != 0) {
			if (!NameFromLock(lock, buf, (LONG)sizeof(buf))) buf[0] = '\0';
			UnLock(lock);
		}
		resolved = 1;
	}

	return (buf[0] != '\0') ? buf : 0;
}

/* Put a fatal error where the player can actually see it.
 *
 * Before this, a startup failure printed one line to stdout - which the run
 * script sends to a file - and then the program simply exited. From the outside
 * that is a game that does not start, with no explanation whatsoever; the user
 * hit exactly this with a stale set of language files and had nothing to go on.
 * An Intuition requester costs nothing and turns it into a sentence.
 *
 * It lives here for the usual reason: this is the one plain-C file allowed the
 * <proto/*> headers. IntuitionBase is opened for us by the C library at
 * startup, so it is available even this early - but it is still checked,
 * because a NULL base is better handled than crashed on. */
void AmigaErrorRequester(const char *text)
{
	struct EasyStruct es;

	if (IntuitionBase == 0 || text == 0) return;

	es.es_StructSize   = sizeof(es);
	es.es_Flags        = 0;
	es.es_Title        = (UBYTE *)"AmiTTD";
	es.es_TextFormat   = (UBYTE *)"%s";
	es.es_GadgetFormat = (UBYTE *)"OK";

	EasyRequestArgs(0, &es, 0, (APTR)&text);
}
