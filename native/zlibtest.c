/* Why does OpenTTD report "cannot initialize compressor"?
 *
 * It calls deflateInit(&z, level) and only checks for != Z_OK, so the actual
 * reason is thrown away. This prints the real return code:
 *   -2 Z_STREAM_ERROR   -4 Z_MEM_ERROR   -6 Z_VERSION_ERROR
 * Z_VERSION_ERROR means the header we compiled against disagrees with libz.a
 * (version string or sizeof(z_stream)); Z_MEM_ERROR means allocation failed.
 *
 * Build (in WSL, against the cross-compiled libz.a):
 *   m68k-amigaos-gcc -O1 -m68040 -noixemul -o zlibtest zlibtest.c -lz
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

unsigned long __stack = 1024 * 1024;

int main(void)
{
	z_stream z;
	int rc;
	static unsigned char src[4096];
	static unsigned char dst[8192];
	static unsigned char back[4096];
	uLongf dlen, blen;
	int i;

	printf("zlibtest: header ZLIB_VERSION = %s\n", ZLIB_VERSION);
	printf("zlibtest: library zlibVersion() = %s\n", zlibVersion());
	printf("zlibtest: sizeof(z_stream) = %d\n", (int)sizeof(z_stream));

	/* zlibCompileFlags() reports the type sizes the LIBRARY was built with;
	 * compare them against what this translation unit thinks. A mismatch means
	 * the header and libz.a disagree on the ABI, which makes every entry point
	 * bail out with Z_STREAM_ERROR. */
	{
		uLong f = zlibCompileFlags();
		printf("zlibtest: compile flags = 0x%08lx\n", (unsigned long)f);
		printf("zlibtest:   lib  sizeof: uInt=%d uLong=%d voidpf=%d z_off_t=%d\n",
		       1 << ((int)(f & 3) + 1),
		       1 << ((int)((f >> 2) & 3) + 1),
		       1 << ((int)((f >> 4) & 3) + 1),
		       1 << ((int)((f >> 6) & 3) + 1));
		printf("zlibtest:   here sizeof: uInt=%d uLong=%d voidpf=%d z_off_t=%d\n",
		       (int)sizeof(uInt), (int)sizeof(uLong),
		       (int)sizeof(voidpf), (int)sizeof(z_off_t));
	}
	printf("zlibtest: MAX_WBITS=%d MAX_MEM_LEVEL=%d Z_DEFLATED=%d\n",
	       MAX_WBITS, MAX_MEM_LEVEL, Z_DEFLATED);
	fflush(stdout);

	/* exactly what OpenTTD's InitWriteZlib does */
	memset(&z, 0, sizeof(z));
	rc = deflateInit(&z, 6);
	printf("zlibtest: deflateInit(level 6) = %d %s\n", rc,
	       rc == Z_OK ? "(Z_OK)" :
	       rc == Z_MEM_ERROR ? "(Z_MEM_ERROR - allocation failed)" :
	       rc == Z_VERSION_ERROR ? "(Z_VERSION_ERROR - header/lib mismatch)" :
	       rc == Z_STREAM_ERROR ? "(Z_STREAM_ERROR - bad parameter)" : "(?)");
	fflush(stdout);
	if (rc == Z_OK) deflateEnd(&z);

	memset(&z, 0, sizeof(z));
	rc = inflateInit(&z);
	printf("zlibtest: inflateInit() = %d\n", rc);
	fflush(stdout);
	if (rc == Z_OK) inflateEnd(&z);

	/* a real round trip, which is what actually matters */
	for (i = 0; i < (int)sizeof(src); i++) src[i] = (unsigned char)(i * 7 + (i >> 3));
	dlen = sizeof(dst);
	rc = compress(dst, &dlen, src, sizeof(src));
	printf("zlibtest: compress() = %d, %d -> %d bytes\n", rc, (int)sizeof(src), (int)dlen);
	fflush(stdout);

	if (rc == Z_OK) {
		blen = sizeof(back);
		rc = uncompress(back, &blen, dst, dlen);
		printf("zlibtest: uncompress() = %d, %d bytes back\n", rc, (int)blen);
		if (rc == Z_OK && blen == sizeof(src) && memcmp(src, back, sizeof(src)) == 0)
			printf("zlibtest: ROUND TRIP OK - zlib works\n");
		else
			printf("zlibtest: ROUND TRIP MISMATCH\n");
	}
	fflush(stdout);

	printf("zlibtest: DONE\n");
	fflush(stdout);
	return 0;
}
