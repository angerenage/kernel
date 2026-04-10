#include <core/kheap.h>
#include <core/lock.h>
#include <core/math.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KHEAP_ALIGN 16ull
#define KHEAP_USED_FLAG ((size_t)1u)
#define KHEAP_SIZE_MASK (~(size_t)(KHEAP_ALIGN - 1u))
#define KHEAP_DEFAULT_GROW_PAGES 4u

struct kheap_block {
	size_t              size_and_flags;
	struct kheap_block* prev_free;
	struct kheap_block* next_free;
	uint64_t            reserved;
};

static const size_t kheap_header_size    = sizeof(struct kheap_block);
static const size_t kheap_footer_size    = sizeof(size_t);
static const size_t kheap_sentinel_size  = 48u;
static const size_t kheap_min_block_size = 64u;

static struct kheap_block* free_list;
static size_t              total_bytes;
static size_t              free_bytes;
static bool                initialized;
static struct spinlock     kheap_lock = SPINLOCK_INIT_CLASS("kheap_lock", SPINLOCK_ORDER_KHEAP, SPINLOCK_FLAG_NONE);

static inline size_t block_size(const struct kheap_block* block) {
	return block->size_and_flags & KHEAP_SIZE_MASK;
}

static inline bool block_used(const struct kheap_block* block) {
	return (block->size_and_flags & KHEAP_USED_FLAG) != 0;
}

static void write_footer(struct kheap_block* block) {
	size_t* footer = (size_t*)((uint8_t*)block + block_size(block) - kheap_footer_size);
	*footer        = block->size_and_flags;
}

static void mark_block(struct kheap_block* block, size_t size, bool used) {
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

static void remove_free_block(struct kheap_block* block) {
	if (block->prev_free != NULL) block->prev_free->next_free = block->next_free;
	else free_list = block->next_free;

	if (block->next_free != NULL) block->next_free->prev_free = block->prev_free;

	block->prev_free = NULL;
	block->next_free = NULL;
}

static void insert_free_block(struct kheap_block* block) {
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

static struct kheap_block* coalesce_block(struct kheap_block* block) {
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

static bool grow_heap(size_t min_block_size) {
	void*  region = NULL;
	size_t request_bytes;
	size_t grow_pages;

	request_bytes = min_block_size + 2u * kheap_sentinel_size;
	grow_pages    = (request_bytes + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;
	if (grow_pages < KHEAP_DEFAULT_GROW_PAGES) grow_pages = KHEAP_DEFAULT_GROW_PAGES;

	if (!kheap_grow_pages(grow_pages, &region)) return false;
	spinlock_lock(&kheap_lock);
	if (!add_arena_locked(region, grow_pages * PMM_PAGE_SIZE)) {
		spinlock_unlock(&kheap_lock);
		return false;
	}
	spinlock_unlock(&kheap_lock);
	return true;
}

static struct kheap_block* find_fit_locked(size_t block_bytes) {
	struct kheap_block* block = free_list;

	while (block != NULL) {
		if (block_size(block) >= block_bytes) return block;
		block = block->next_free;
	}

	return NULL;
}

bool kheap_init(void) {
	spinlock_lock(&kheap_lock);
	free_list   = NULL;
	total_bytes = 0;
	free_bytes  = 0;
	initialized = false;
	spinlock_unlock(&kheap_lock);

	if (!grow_heap(kheap_min_block_size)) return false;

	spinlock_lock(&kheap_lock);
	initialized = true;
	spinlock_unlock(&kheap_lock);
	return true;
}

bool kheap_is_initialized(void) {
	return initialized;
}

void* kmalloc(size_t size) {
	struct kheap_block* block;
	struct kheap_block* remainder;
	size_t              payload_bytes;
	size_t              block_bytes;
	size_t              remainder_bytes;

	if (!initialized || size == 0) return NULL;
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
		spinlock_lock(&kheap_lock);
		block = find_fit_locked(block_bytes);
		if (block != NULL) break;
		spinlock_unlock(&kheap_lock);
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
	spinlock_unlock(&kheap_lock);
	return ptr;
}

void kfree(void* ptr) {
	struct kheap_block* block;

	if (!initialized || ptr == NULL) return;
	spinlock_lock(&kheap_lock);

	block = (struct kheap_block*)((uint8_t*)ptr - kheap_header_size);
	if (!block_used(block)) {
		spinlock_unlock(&kheap_lock);
		return;
	}

	mark_block(block, block_size(block), false);
	free_bytes += block_size(block);
	block = coalesce_block(block);
	insert_free_block(block);
	spinlock_unlock(&kheap_lock);
}

void* kcalloc(size_t nmemb, size_t size) {
	void*  ptr;
	size_t total;

	if (mul_overflow_size(nmemb, size, &total)) return NULL;

	ptr = kmalloc(total);
	if (ptr != NULL) memset(ptr, 0, total);
	return ptr;
}

void* krealloc(void* ptr, size_t size) {
	struct kheap_block* block;
	size_t              current_payload;
	void*               new_ptr;

	if (ptr == NULL) return kmalloc(size);
	if (size == 0) {
		kfree(ptr);
		return NULL;
	}

	block = (struct kheap_block*)((uint8_t*)ptr - kheap_header_size);
	if (!block_used(block)) return NULL;

	current_payload = block_size(block) - kheap_header_size - kheap_footer_size;
	if (size <= current_payload) return ptr;

	new_ptr = kmalloc(size);
	if (!new_ptr) return NULL;

	memcpy(new_ptr, ptr, current_payload);
	kfree(ptr);
	return new_ptr;
}

size_t kheap_total_bytes(void) {
	size_t total;

	spinlock_lock(&kheap_lock);
	total = total_bytes;
	spinlock_unlock(&kheap_lock);
	return total;
}

size_t kheap_free_bytes(void) {
	size_t free_now;

	spinlock_lock(&kheap_lock);
	free_now = free_bytes;
	spinlock_unlock(&kheap_lock);
	return free_now;
}
