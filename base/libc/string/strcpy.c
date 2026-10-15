#include <string.h>

char* strcpy(char* restrict destination, const char* restrict source) {
	char* cursor = destination;
	while ((*cursor++ = *source++) != '\0') {
	}
	return destination;
}
