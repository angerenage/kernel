#include <libc/string.h>
#include <stddef.h>

char* strchr(const char* string, int character) {
	char target = (char)character;
	for (;;) {
		if (*string == target) return (char*)string;
		if (*string == '\0') return NULL;
		string++;
	}
}
