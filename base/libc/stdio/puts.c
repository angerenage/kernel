#include <libc/stdio.h>

int puts(const char* str) {
	const char* cursor = str;
	int         count  = 0;

	if (!cursor) return EOF;

	while (*cursor) {
		if (putchar(*cursor++) == EOF) return EOF;
		count++;
	}
	putchar('\n');
	return count + 1;
}
