#include <libc/string.h>

int strncmp(const char* left, const char* right, size_t count) {
	for (size_t index = 0u; index < count; index++) {
		unsigned char left_byte  = (unsigned char)left[index];
		unsigned char right_byte = (unsigned char)right[index];

		if (left_byte != right_byte) return left_byte < right_byte ? -1 : 1;
		if (left_byte == '\0') return 0;
	}
	return 0;
}
