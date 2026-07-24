/* AmigaOS directory scanner for the native music driver.
 *
 * This deliberately stays plain C.  AmigaOS <proto/*> headers collide with
 * an OpenTTD C++ Point declaration, so no OpenTTD header may be included
 * here and no AmigaOS header may leak through amiga_mscan.h.
 */

#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <stddef.h>
#include <string.h>

#include "amiga_mscan.h"

static int AsciiLower(int c)
{
	if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
	return c;
}

static int IsWaveFile(const char *name, size_t len)
{
	if (len < 5) return 0; /* at least one basename character plus ".wav" */
	return name[len - 4] == '.' &&
			AsciiLower((unsigned char)name[len - 3]) == 'w' &&
			AsciiLower((unsigned char)name[len - 2]) == 'a' &&
			AsciiLower((unsigned char)name[len - 1]) == 'v';
}

int AmigaMusic_ScanDir(const char *dir,
		char paths[][AMIGA_MUSIC_PATH_MAX],
		char names[][AMIGA_MUSIC_NAME_MAX], int max)
{
	BPTR lock;
	struct FileInfoBlock *fib;
	size_t dir_len;
	int add_slash;
	int count;

	if (dir == NULL || paths == NULL || names == NULL || max <= 0) return 0;

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
			 * Dotfiles and non-WAV entries are not part of the music set. */
			if (fib->fib_DirEntryType >= 0) continue;
			if (filename[0] == '.') continue;
			if (!IsWaveFile(filename, file_len)) continue;

			if (dir_len + (add_slash ? 1 : 0) + file_len + 1 >
					AMIGA_MUSIC_PATH_MAX) {
				continue;
			}

			memcpy(paths[count], dir, dir_len);
			pos = dir_len;
			if (add_slash) paths[count][pos++] = '/';
			memcpy(paths[count] + pos, filename, file_len + 1);

			base_len = file_len - 4;
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
