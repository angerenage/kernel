#include <string.h>

void* memmove(void* dest, const void* src, size_t count) {
	unsigned char*       dest_bytes = dest;
	const unsigned char* src_bytes  = src;

	if (dest_bytes == src_bytes || count == 0) return dest;

	if (dest_bytes < src_bytes) {
		for (size_t i = 0; i < count; i++) {
			dest_bytes[i] = src_bytes[i];
		}
	}
	else {
		for (size_t i = count; i > 0; i--) {
			dest_bytes[i - 1] = src_bytes[i - 1];
		}
	}

	return dest;
}
