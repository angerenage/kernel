#include <core/kheap.h>
#include <string.h>

char* strdup(const char* str) {
	size_t length;
	char*  copy;

	length = strlen(str) + 1u;
	copy   = kmalloc(length);
	if (copy == NULL) return NULL;

	memcpy(copy, str, length);
	return copy;
}
