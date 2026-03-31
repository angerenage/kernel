#include <string.h>

int memcmp(const void* lhs, const void* rhs, size_t count) {
	const unsigned char* lhs_bytes = lhs;
	const unsigned char* rhs_bytes = rhs;

	for (size_t i = 0; i < count; i++) {
		if (lhs_bytes[i] != rhs_bytes[i]) {
			return (lhs_bytes[i] < rhs_bytes[i]) ? -1 : 1;
		}
	}

	return 0;
}
