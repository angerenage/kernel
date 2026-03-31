#include <stdio.h>

#include "format.h"

void sprintf(char* buffer, const char* restrict format, ...) {
	if (!buffer) return;

	va_list args;
	va_start(args, format);
	format_to_buffer(buffer, format, &args);
	va_end(args);
}
