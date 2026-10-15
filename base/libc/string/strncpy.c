#include <string.h>

char* strncpy(char* restrict destination, const char* restrict source, size_t count) {
	size_t index = 0u;
	while (index < count && source[index] != '\0') {
		destination[index] = source[index];
		index++;
	}
	while (index < count) destination[index++] = '\0';
	return destination;
}
