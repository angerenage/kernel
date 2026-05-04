#include <base/heap.h>
#include <libc/stdlib.h>
#include <string.h>

void* realloc(void* ptr, size_t size) {
	struct kheap_block* block;
	size_t              current_payload;
	void*               new_ptr;

	if (ptr == NULL) return malloc(size);
	if (size == 0) {
		free(ptr);
		return NULL;
	}

	block = (struct kheap_block*)((uint8_t*)ptr - kheap_header_size);
	if (!block_used(block)) return NULL;

	current_payload = block_size(block) - kheap_header_size - kheap_footer_size;
	if (size <= current_payload) return ptr;

	new_ptr = malloc(size);
	if (!new_ptr) return NULL;

	memcpy(new_ptr, ptr, current_payload);
	free(ptr);
	return new_ptr;
}
