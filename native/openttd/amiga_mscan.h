/* Plain-C AmigaDOS directory scanner used by the native music driver.
 *
 * Keep this header independent of both AmigaOS and OpenTTD headers.  The
 * implementation is the only music-side file which includes <proto/dos.h>.
 */
#ifndef AMIGA_MSCAN_H
#define AMIGA_MSCAN_H

#define AMIGA_MUSIC_PATH_MAX 256
#define AMIGA_MUSIC_NAME_MAX 64

#ifdef __cplusplus
extern "C" {
#endif

/* Scan one full AmigaDOS directory path (for example
 * "PROGDIR:music/Nowe").  Only regular, non-dot *.wav entries are returned.
 * The suffix comparison is ASCII case-insensitive.  paths[] receives the
 * complete path and names[] the filename without its final .wav suffix.
 *
 * Entries whose complete path does not fit are skipped.  Missing/unreadable
 * directories and allocation failures return zero.
 */
int AmigaMusic_ScanDir(const char *dir,
		char paths[][AMIGA_MUSIC_PATH_MAX],
		char names[][AMIGA_MUSIC_NAME_MAX], int max);

/* The same scan with the suffix spelled out, for the MIDI sources: ".mid" for
 * the OpenMSX set we ship and ".gm" for the player's own original music. The
 * comparison is ASCII case-insensitive and names[] drops the suffix. */
int AmigaMusic_ScanDirExt(const char *dir, const char *ext,
		char paths[][AMIGA_MUSIC_PATH_MAX],
		char names[][AMIGA_MUSIC_NAME_MAX], int max);


/* Resolve PROGDIR: to a real AmigaDOS path ("Work:Games/OpenTTD"), or NULL.
 * Used by the file layer so the game finds its own lang/ and data/ whatever
 * the current directory happens to be - see the comment in the .c file.
 */
const char *AmigaProgDirPath(void);

/* Send stdout/stderr to NIL: when the program was started from Workbench,
 * so double-clicking the icon does not open a console full of debug text. */
void AmigaSilenceIfWorkbench(void);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_MSCAN_H */
