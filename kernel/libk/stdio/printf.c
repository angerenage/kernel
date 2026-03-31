#include <stdio.h>

#include "../format/format.h"

void printf(const char* restrict format, ...) {
	va_list args;
	va_start(args, format);
	format_to_serial(format, &args);
	va_end(args);
}
