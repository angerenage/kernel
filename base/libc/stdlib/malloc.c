#include <base/heap.h>
#include <base/math.h>
#include <libc/stdlib.h>

void* malloc(size_t size) {
	struct kheap_block* block;
	struct kheap_block* remainder;
	size_t              payload_bytes;
	size_t              block_bytes;
	size_t              remainder_bytes;

	if (!heap_is_initialized() || size == 0) return NULL;
	if (!align_up_size(size, KHEAP_ALIGN, &payload_bytes)) {
		return NULL;
	}
	if (add_overflow_size(kheap_header_size + kheap_footer_size, payload_bytes, &block_bytes)) {
		return NULL;
	}
	if (!align_up_size(block_bytes, KHEAP_ALIGN, &block_bytes)) {
		return NULL;
	}
	if (block_bytes < kheap_min_block_size) block_bytes = kheap_min_block_size;

	for (;;) {
		heap_lock();
		block = find_fit_locked(block_bytes);
		if (block != NULL) break;
		heap_unlock();
		if (!grow_heap(block_bytes)) return NULL;
	}

	remove_free_block(block);
	remainder_bytes = block_size(block) - block_bytes;

	if (remainder_bytes >= kheap_min_block_size) {
		remainder = (struct kheap_block*)((uint8_t*)block + block_bytes);
		mark_block(remainder, remainder_bytes, false);
		insert_free_block(remainder);
		mark_block(block, block_bytes, true);
	}
	else {
		block_bytes = block_size(block);
		mark_block(block, block_bytes, true);
	}

	free_bytes -= block_bytes;
	void* ptr = (uint8_t*)block + kheap_header_size;
	heap_unlock();
	return ptr;
}
