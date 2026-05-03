#include <core/kheap.h>
#include <libk/string.h>

char* strndup(const char* str, size_t size) {
	size_t length = 0u;
	char*  copy;

	while (length < size && str[length] != '\0') {
		length++;
	}

	copy = kmalloc(length + 1u);
	if (copy == NULL) return NULL;

	memcpy(copy, str, length);
	copy[length] = '\0';
	return copy;
}
