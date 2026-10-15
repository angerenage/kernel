#include <libc/string.h>
#include <stddef.h>

char* strstr(const char* haystack, const char* needle) {
	if (*needle == '\0') return (char*)haystack;
	size_t needle_size = strlen(needle);
	for (; *haystack != '\0'; haystack++)
		if (*haystack == *needle && strncmp(haystack, needle, needle_size) == 0) return (char*)haystack;
	return NULL;
}
