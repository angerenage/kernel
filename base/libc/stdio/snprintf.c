#include <libc/stdio.h>

#include "format.h"

int vsnprintf(char* restrict buffer, size_t size, const char* restrict format, va_list arguments) {
	va_list copy;
	va_copy(copy, arguments);
	int result = format_to_buffer_size(buffer, size, format, &copy);
	va_end(copy);
	return result;
}

int snprintf(char* restrict buffer, size_t size, const char* restrict format, ...) {
	va_list arguments;
	va_start(arguments, format);
	int result = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return result;
}
