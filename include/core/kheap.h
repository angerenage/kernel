#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Simple kernel heap allocator layered on top of page-based heap growth.
 *
 * The allocator manages one or more arenas obtained from kheap_grow_pages(),
 * splits/coalesces blocks, and provides malloc-family helpers for internal
 * kernel allocations.
 */

/* Initialize heap metadata and request the first arena from the page supplier. */
bool kheap_init(void);

/* Return whether kheap_init() has completed successfully. */
bool kheap_is_initialized(void);

/* Allocate a heap block large enough for size bytes, or NULL on failure. */
void* kmalloc(size_t size);

/* Free a block previously returned by kmalloc/kcalloc/krealloc. */
void kfree(void* ptr);

/* Allocate nmemb * size bytes and zero-initialize the resulting block. */
void* kcalloc(size_t nmemb, size_t size);

/* Grow or move an allocation while preserving the old contents up to the old payload size. */
void* krealloc(void* ptr, size_t size);

/* Backend hook that appends page-aligned heap memory and reports the virtual base of the new arena. */
bool kheap_grow_pages(size_t page_count, void** out_base);

/* Total usable bytes currently managed by the heap across all arenas. */
size_t kheap_total_bytes(void);

/* Current free-byte estimate inside the managed heap arenas. */
size_t kheap_free_bytes(void);
