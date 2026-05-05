#include <base/display.h>
#include <libc/stdio.h>

int putchar(int ch) {
	char out = (char)ch;

	base_display_write(&out, 1u);
	return (unsigned char)out;
}
