#include <base/heap.h>

bool heap_grow_region(size_t minimum_size, void** out_base, size_t* out_size) {
	(void)minimum_size;
	if (out_base != NULL) *out_base = NULL;
	if (out_size != NULL) *out_size = 0u;
	return false;
}
