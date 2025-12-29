#include <stdarg.h>
#include <stdio.h>

#include "utils.h"

void puts(const char* str) {
	if (!str) return;

	while (*str) serial_out_char(*str++);
	serial_out_char('\n');
}
