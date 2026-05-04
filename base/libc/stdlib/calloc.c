#include <base/heap.h>
#include <base/math.h>
#include <libc/stdlib.h>
#include <string.h>

void* calloc(size_t nmemb, size_t size) {
	void*  ptr;
	size_t total;

	if (mul_overflow_size(nmemb, size, &total)) return NULL;

	ptr = malloc(total);
	if (ptr != NULL) memset(ptr, 0, total);
	return ptr;
}
