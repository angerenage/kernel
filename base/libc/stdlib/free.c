#include <base/heap.h>
#include <libc/stdlib.h>

void free(void* ptr) {
	struct heap_block* block;

	if (!heap_is_initialized() || ptr == NULL) return;
	heap_lock();

	block = (struct heap_block*)((uint8_t*)ptr - heap_header_size);
	if (!block_used(block)) {
		heap_unlock();
		return;
	}

	mark_block(block, block_size(block), false);
	free_bytes += block_size(block);
	block = coalesce_block(block);
	insert_free_block(block);
	heap_unlock();
}
