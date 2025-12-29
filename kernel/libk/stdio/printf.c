#include <stdarg.h>
#include <stdio.h>

#include "utils.h"

void printf(const char* __restrict format, ...) {
	va_list args;
	va_start(args, format);
	format_emit(serial_emit_adapter, NULL, format, &args);
	va_end(args);
}
