#include <string.h>

void* memchr(const void* ptr, int value, size_t count) {
	const unsigned char* bytes  = ptr;
	const unsigned char  target = (unsigned char)value;

	for (size_t i = 0u; i < count; i++) {
		if (bytes[i] == target) return (void*)(bytes + i);
	}

	return NULL;
}
