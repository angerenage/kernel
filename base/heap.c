#include <base/heap.h>
#include <base/math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct kheap_block* free_list;
size_t              total_bytes;
size_t              free_bytes;
bool                initialized;

size_t block_size(const struct kheap_block* block) {
	return block->size_and_flags & KHEAP_SIZE_MASK;
}

bool block_used(const struct kheap_block* block) {
	return (block->size_and_flags & KHEAP_USED_FLAG) != 0;
}

static void write_footer(struct kheap_block* block) {
	size_t* footer = (size_t*)((uint8_t*)block + block_size(block) - kheap_footer_size);
	*footer        = block->size_and_flags;
}

void mark_block(struct kheap_block* block, size_t size, bool used) {
	block->size_and_flags = size | (used ? KHEAP_USED_FLAG : 0u);
	if (!used) {
		block->prev_free = NULL;
		block->next_free = NULL;
	}
	write_footer(block);
}

static inline struct kheap_block* next_block(const struct kheap_block* block) {
	return (struct kheap_block*)((uint8_t*)block + block_size(block));
}

static inline struct kheap_block* prev_block(const struct kheap_block* block) {
	const size_t* footer    = (const size_t*)((const uint8_t*)block - kheap_footer_size);
	size_t        prev_size = *footer & KHEAP_SIZE_MASK;
	return (struct kheap_block*)((uint8_t*)block - prev_size);
}

void remove_free_block(struct kheap_block* block) {
	if (block->prev_free != NULL) block->prev_free->next_free = block->next_free;
	else free_list = block->next_free;

	if (block->next_free != NULL) block->next_free->prev_free = block->prev_free;

	block->prev_free = NULL;
	block->next_free = NULL;
}

void insert_free_block(struct kheap_block* block) {
	struct kheap_block* current = free_list;
	struct kheap_block* prev    = NULL;

	block->prev_free = NULL;
	block->next_free = NULL;

	while (current != NULL && current < block) {
		prev    = current;
		current = current->next_free;
	}

	block->prev_free = prev;
	block->next_free = current;

	if (prev != NULL) prev->next_free = block;
	else free_list = block;

	if (current != NULL) current->prev_free = block;
}

struct kheap_block* coalesce_block(struct kheap_block* block) {
	struct kheap_block* right = next_block(block);
	struct kheap_block* left;

	if (!block_used(right)) {
		remove_free_block(right);
		mark_block(block, block_size(block) + block_size(right), false);
	}

	left = prev_block(block);
	if (!block_used(left)) {
		remove_free_block(left);
		mark_block(left, block_size(left) + block_size(block), false);
		block = left;
	}

	return block;
}

static bool add_arena_locked(void* base, size_t size_bytes) {
	struct kheap_block* prologue;
	struct kheap_block* free_block;
	struct kheap_block* epilogue;
	size_t              free_block_size;

	if (!base) return false;
	if (size_bytes < (2u * kheap_sentinel_size + kheap_min_block_size)) return false;
	if ((size_bytes & (KHEAP_ALIGN - 1u)) != 0) return false;

	prologue        = (struct kheap_block*)base;
	free_block      = (struct kheap_block*)((uint8_t*)base + kheap_sentinel_size);
	free_block_size = size_bytes - 2u * kheap_sentinel_size;
	epilogue        = (struct kheap_block*)((uint8_t*)free_block + free_block_size);

	mark_block(prologue, kheap_sentinel_size, true);
	mark_block(free_block, free_block_size, false);
	mark_block(epilogue, kheap_sentinel_size, true);

	free_block = coalesce_block(free_block);
	insert_free_block(free_block);

	total_bytes += free_block_size;
	free_bytes += free_block_size;
	return true;
}

bool grow_heap(size_t min_block_size) {
	void*  region = NULL;
	size_t page_size;
	size_t request_bytes;
	size_t grow_pages;

	page_size = heap_page_size();
	if (page_size == 0u) return false;

	request_bytes = min_block_size + 2u * kheap_sentinel_size;
	grow_pages    = (request_bytes + page_size - 1u) / page_size;
	if (grow_pages < KHEAP_DEFAULT_GROW_PAGES) grow_pages = KHEAP_DEFAULT_GROW_PAGES;

	if (!heap_grow_pages(grow_pages, &region)) return false;
	heap_lock();
	if (!add_arena_locked(region, grow_pages * page_size)) {
		heap_unlock();
		return false;
	}
	heap_unlock();
	return true;
}

struct kheap_block* find_fit_locked(size_t block_bytes) {
	struct kheap_block* block = free_list;

	while (block != NULL) {
		if (block_size(block) >= block_bytes) return block;
		block = block->next_free;
	}

	return NULL;
}

bool heap_init(void) {
	heap_lock();
	free_list   = NULL;
	total_bytes = 0;
	free_bytes  = 0;
	initialized = false;
	heap_unlock();

	if (!grow_heap(kheap_min_block_size)) return false;

	heap_lock();
	initialized = true;
	heap_unlock();
	return true;
}

bool heap_is_initialized(void) {
	bool is_initialized;

	heap_lock();
	is_initialized = initialized;
	heap_unlock();
	return is_initialized;
}

size_t heap_total_bytes(void) {
	size_t total;

	heap_lock();
	total = total_bytes;
	heap_unlock();
	return total;
}

size_t heap_free_bytes(void) {
	size_t free_now;

	heap_lock();
	free_now = free_bytes;
	heap_unlock();
	return free_now;
}
