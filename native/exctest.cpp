/* Does C++ exception unwinding actually work in this toolchain configuration?
 *
 * OpenTTD relies on throw/catch (SlError, oldloader), and in our build an
 * exception blew straight past a catch(...) into std::terminate. That means the
 * unwinder never found the handler - phase 1 of unwinding failed, so it
 * terminated without unwinding at all. This isolates that from OpenTTD.
 *
 * Build with EXACTLY the flags the game uses:
 *   m68k-amigaos-g++ -m68040 -O2 -noixemul -std=gnu++98 -fpermissive -o exctest exctest.cpp
 */
#include <stdio.h>
#include <exception>

unsigned long __stack = 1024 * 1024;

static void thrower(int depth)
{
	if (depth > 0) { thrower(depth - 1); return; }
	throw std::exception();
}

int main(void)
{
	printf("exctest: start\n"); fflush(stdout);

	/* 1: throw and catch in the same function */
	try {
		throw std::exception();
	} catch (...) {
		printf("exctest: OK - caught a same-function throw\n"); fflush(stdout);
	}

	/* 2: throw several frames deep, which is what OpenTTD actually does */
	try {
		thrower(3);
		printf("exctest: FAIL - thrower() returned without throwing\n"); fflush(stdout);
	} catch (std::exception &) {
		printf("exctest: OK - caught std::exception thrown 4 frames deep\n"); fflush(stdout);
	} catch (...) {
		printf("exctest: OK - caught something 4 frames deep\n"); fflush(stdout);
	}

	printf("exctest: DONE - exceptions work\n"); fflush(stdout);
	return 0;
}
