#include <base/heap.h>
#include <libc/stdlib.h>
#include <string.h>

void* realloc(void* ptr, size_t size) {
	struct heap_block* block;
	size_t             current_payload;
	void*              new_ptr;

	if (ptr == NULL) return malloc(size);
	if (size == 0) {
		free(ptr);
		return NULL;
	}

	block = (struct heap_block*)((uint8_t*)ptr - heap_header_size);
	if (!block_used(block)) return NULL;

	current_payload = block_size(block) - heap_header_size - heap_footer_size;
	if (size <= current_payload) return ptr;

	new_ptr = malloc(size);
	if (!new_ptr) return NULL;

	memcpy(new_ptr, ptr, current_payload);
	free(ptr);
	return new_ptr;
}
