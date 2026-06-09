#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allocated block alignment enforced by the heap allocator. */
#define HEAP_ALIGN 16ull
#define HEAP_USED_FLAG ((size_t)1u)
#define HEAP_SIZE_MASK (~(size_t)(HEAP_ALIGN - 1u))
/* Default number of pages added to the heap when it needs to grow. */
#define HEAP_DEFAULT_GROW_PAGES 4u

/* One free or allocated chunk in the free-list heap. */
struct heap_block {
	size_t             size_and_flags;
	struct heap_block* prev_free;
	struct heap_block* next_free;
	uint64_t           reserved;
};

static const size_t heap_header_size = sizeof(struct heap_block);
static const size_t heap_footer_size = sizeof(size_t);
/* Bytes reserved at the start and end of every arena for prologue/epilogue sentinels. */
static const size_t heap_sentinel_size = 48u;
/* Minimum size of a free block, including header/footer overhead. */
static const size_t heap_min_block_size = 64u;

extern struct heap_block* free_list;
extern size_t             total_bytes;
extern size_t             free_bytes;

/* Initialize the heap and seed it with a first arena. Safe to call only once. */
bool heap_init(void);

/* Return true once heap_init() has completed successfully. */
bool heap_is_initialized(void);

/* Total bytes currently under heap management, including headers and sentinels. */
size_t heap_total_bytes(void);

/* Bytes currently available for allocation, summed across every arena. */
size_t heap_free_bytes(void);

/* Return the usable byte size stored in a block header. */
size_t block_size(const struct heap_block* block);

/* Return true when a block currently holds an allocation. */
bool block_used(const struct heap_block* block);

/* Rewrite a block header (and matching footer) for the supplied size and used state. */
void mark_block(struct heap_block* block, size_t size, bool used);

/* Merge block with adjacent free blocks and return the resulting base block. */
struct heap_block* coalesce_block(struct heap_block* block);

/* Unlink a block from the global free list. */
void remove_free_block(struct heap_block* block);

/* Insert a block into the address-ordered free list. */
void insert_free_block(struct heap_block* block);

/* Grow the heap by adding a new arena large enough to satisfy a min_block_size request. */
bool grow_heap(size_t min_block_size);

/* Return the first free block large enough for block_bytes; caller must hold the heap lock. */
struct heap_block* find_fit_locked(size_t block_bytes);

/* Add page_count freshly-allocated pages to the heap arena, returning the arena base. */
bool heap_grow_pages(size_t page_count, void** out_base);

/* Return the platform page size used for arena growth. */
size_t heap_page_size(void);

/* Acquire the global heap lock. */
void heap_lock(void);

/* Release the global heap lock. */
void heap_unlock(void);
