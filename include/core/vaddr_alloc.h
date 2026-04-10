#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Bitmap allocator for reserving virtual address ranges inside a preselected
 * window. This layer only tracks address space ownership; it does not create
 * page-table mappings.
 */

/* Initialize the allocator over [base, base + page_count * PMM_PAGE_SIZE). */
bool vaddr_alloc_init(uintptr_t base, size_t page_count);

/* Return whether the allocator has been initialized. */
bool vaddr_alloc_is_initialized(void);

/* Reserve count consecutive virtual pages with the requested page alignment. */
bool vaddr_alloc_reserve(size_t count, size_t align_pages, uintptr_t* out_base);

/* Release a previously reserved page range back to the virtual window. */
bool vaddr_alloc_release(uintptr_t base, size_t count);

/* Total number of pages tracked by the allocator. */
size_t vaddr_alloc_total_page_count(void);

/* Number of currently unreserved pages in the virtual window. */
size_t vaddr_alloc_free_page_count(void);
