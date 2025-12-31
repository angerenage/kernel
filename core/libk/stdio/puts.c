#include <stdio.h>
#include <stdio/utils.h>

void puts(const char* str) {
	if (!str) return;

	while (*str) serial_out_char(*str++);
	serial_out_char('\n');
}
