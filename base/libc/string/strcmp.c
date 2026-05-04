#include <string.h>

int strcmp(const char* lhs, const char* rhs) {
	while (*lhs != '\0' && *lhs == *rhs) {
		lhs++;
		rhs++;
	}

	return (unsigned char)*lhs - (unsigned char)*rhs;
}
