#include <libc/stdio.h>

#include "format.h"

int printf(const char* restrict format, ...) {
	int result;

	va_list args;
	va_start(args, format);
	result = format_to_display(format, &args);
	va_end(args);
	return result;
}
