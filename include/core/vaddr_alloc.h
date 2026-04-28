#pragma once

#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vmm_alloc_record;

/*
 * Bitmap allocator for reserving virtual address ranges inside an address
 * space window. This layer only tracks address ownership; it does not create
 * page-table mappings.
 */

struct address_space {
	uintptr_t                base;
	struct hal_address_space hal_space;
	uint64_t*                bitmap;
	uintptr_t                bitmap_phys;
	size_t                   bitmap_pages;
	size_t                   total_pages;
	size_t                   free_pages;
	struct vmm_alloc_record* allocations;
	uintptr_t                allocations_phys;
	size_t                   allocations_page_count;
	size_t                   allocations_capacity;
	size_t                   allocation_count;
	uint64_t                 next_allocation_id;
	bool                     initialized;
	struct spinlock          lock;
};

/* Initialize an address-space allocator over [base, base + page_count * PMM_PAGE_SIZE). */
bool address_space_init(struct address_space* space, uintptr_t base, size_t page_count);

/* Release allocator metadata and mark the address-space allocator uninitialized. */
void address_space_deinit(struct address_space* space);

/* Return whether an address-space allocator has been initialized. */
bool address_space_is_initialized(const struct address_space* space);

/* Return the architecture paging handle backing an initialized address space. */
struct hal_address_space* address_space_hal(struct address_space* space);

/* Switch the current CPU to an initialized address space's paging handle. */
bool address_space_activate(struct address_space* space);

/* Reserve count consecutive virtual pages with the requested page alignment. */
bool address_space_reserve(struct address_space* space, size_t count, size_t align_pages, uintptr_t* out_base);

/* Release a previously reserved page range back to the address-space window. */
bool address_space_release(struct address_space* space, uintptr_t base, size_t count);

/* Total number of pages tracked by an address-space allocator. */
size_t address_space_total_page_count(struct address_space* space);

/* Number of currently unreserved pages in an address-space allocator. */
size_t address_space_free_page_count(struct address_space* space);

/* Return the kernel address-space allocator. */
struct address_space* address_space_kernel(void);
