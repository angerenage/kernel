#include <libc/stdio.h>

#include "format.h"

int sprintf(char* restrict buffer, const char* restrict format, ...) {
	char* out = buffer;
	int   result;

	if (!out) return EOF;

	va_list args;
	va_start(args, format);
	result = format_to_buffer(out, format, &args);
	va_end(args);
	return result;
}
