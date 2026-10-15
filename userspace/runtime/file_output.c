#include <libc/limits.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>

int vfprintf(FILE* restrict stream, const char* restrict format, va_list arguments) {
	if (stream == NULL || format == NULL) return EOF;
	va_list sizing;
	va_copy(sizing, arguments);
	int size = vsnprintf(NULL, 0u, format, sizing);
	va_end(sizing);
	if (size < 0 || size == INT_MAX) return EOF;
	char* buffer = malloc((size_t)size + 1u);
	if (buffer == NULL) return EOF;
	int result = vsnprintf(buffer, (size_t)size + 1u, format, arguments);
	if (result != size || fwrite(buffer, 1u, (size_t)size, stream) != (size_t)size) result = EOF;
	free(buffer);
	return result;
}

int fprintf(FILE* restrict stream, const char* restrict format, ...) {
	va_list arguments;
	va_start(arguments, format);
	int result = vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int fputs(const char* restrict string, FILE* restrict stream) {
	if (string == NULL || stream == NULL) return EOF;
	size_t size = strlen(string);
	return fwrite(string, 1u, size, stream) == size ? 0 : EOF;
}

int putc(int character, FILE* stream) {
	unsigned char byte = (unsigned char)character;
	return fwrite(&byte, 1u, 1u, stream) == 1u ? (int)byte : EOF;
}
