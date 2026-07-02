#include <libc/string.h>

size_t strlcpy(char* restrict destination, const char* restrict source, size_t capacity) {
	size_t length = 0u;

	while (source[length] != '\0') {
		if (length + 1u < capacity) destination[length] = source[length];
		length++;
	}

	if (capacity > 0u) {
		destination[length < capacity ? length : capacity - 1u] = '\0';
	}

	return length;
}
