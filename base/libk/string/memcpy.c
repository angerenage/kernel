#include <string.h>

void* memcpy(void* restrict dest, const void* restrict src, size_t count) {
	unsigned char*       dest_bytes = dest;
	const unsigned char* src_bytes  = src;

	for (size_t i = 0; i < count; i++) {
		dest_bytes[i] = src_bytes[i];
	}

	return dest;
}
