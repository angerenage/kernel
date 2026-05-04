#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

extern struct kheap_block* free_list;
extern size_t              total_bytes;
extern size_t              free_bytes;

bool   heap_init(void);
bool   heap_is_initialized(void);
size_t heap_total_bytes(void);
size_t heap_free_bytes(void);

size_t              block_size(const struct kheap_block* block);
bool                block_used(const struct kheap_block* block);
void                mark_block(struct kheap_block* block, size_t size, bool used);
struct kheap_block* coalesce_block(struct kheap_block* block);
void                remove_free_block(struct kheap_block* block);
void                insert_free_block(struct kheap_block* block);
bool                grow_heap(size_t min_block_size);
struct kheap_block* find_fit_locked(size_t block_bytes);

bool   heap_grow_pages(size_t page_count, void** out_base);
size_t heap_page_size(void);
void   heap_lock(void);
void   heap_unlock(void);
