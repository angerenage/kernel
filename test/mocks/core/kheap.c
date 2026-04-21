#include <core/kheap.h>

__attribute__((weak))
bool kheap_grow_pages(size_t page_count, void** out_base) {
	(void)page_count;
	if (out_base != NULL) *out_base = NULL;
	return false;
}
