#include <string.h>

char* strcat(char* restrict destination, const char* restrict source) {
	char* end = destination + strlen(destination);
	while ((*end++ = *source++) != '\0') {
	}
	return destination;
}
