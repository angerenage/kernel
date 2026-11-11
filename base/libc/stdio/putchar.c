#include <libc/stdio.h>

#include <base/display.h>

int putchar(int ch) {
	char out = (char)ch;

	base_display_write(&out, 1u);
	return (unsigned char)out;
}
