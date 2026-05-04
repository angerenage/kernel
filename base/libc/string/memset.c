#include <string.h>

void* memset(void* dest, int value, size_t count) {
	unsigned char* bytes = dest;

	for (size_t i = 0; i < count; i++) {
		bytes[i] = (unsigned char)value;
	}

	return dest;
}
