#include <string.h>

char* strrchr(const char* string, int character) {
	const char* match  = NULL;
	char        target = (char)character;
	do {
		if (*string == target) match = string;
	} while (*string++ != '\0');
	return (char*)match;
}
