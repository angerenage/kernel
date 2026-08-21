#pragma once

#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/region_presence.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One tracked virtual-memory region owned by an address space. */
struct memory_region {
	vmm_id_t id;
	/* Initially reserved base including guard pages, or 0 if none. */
	uintptr_t reserved_base;
	/* Actual usable base address of the allocation. */
	uintptr_t base;
	/* Total pages reserved including guard pages. */
	size_t reserved_page_count;
	/* Usable page count excluding guard pages. */
	size_t        page_count;
	vmm_prot_t    prot;
	enum vmm_kind kind;
	/* Guard pages below the usable range (stacks only). */
	size_t guard_pages;
	/* Map policy flags (e.g. lazy mapping). */
	uint64_t map_flags;
	/* Retained logical-memory backing and the first object page visible through this mapping. */
	struct memory_object* memory;
	size_t                memory_page_offset;
	/* Sparse per-mapping record of pages with live page-table entries. */
	struct region_presence presence;
	/* Slot validity flag for the region table. */
	bool used;
};

/* Output of a successful memory_region_create() call. */
struct memory_region_create_result {
	struct memory_region* region;
	uintptr_t             base;
};

/* Return true when params describe an allocation the core layer is allowed to create. */
bool memory_region_params_allowed(const struct address_space* space, const struct vmm_alloc_params* params);

/* Create a tracked region following the supplied parameters and report its base. */
bool memory_region_create(struct address_space* space, uintptr_t requested_base, const struct vmm_alloc_params* params,
                          struct memory_region_create_result* out_result);

/* Create a mapping over a validated subrange of an existing Memory Object. */
bool memory_region_create_with_object(struct address_space* space, uintptr_t requested_base,
                                      struct memory_object* memory, size_t memory_page_offset,
                                      const struct vmm_alloc_params*      params,
                                      struct memory_region_create_result* out_result);

/* Create a region backed by externally-owned physical pages. */
bool memory_region_create_phys(struct address_space* space, uintptr_t requested_base, uintptr_t phys_base,
                               const struct vmm_alloc_params* params, struct memory_region** out_region,
                               uintptr_t* out_base);

/* Destroy a single region and release any owned backing pages. */
bool memory_region_destroy(struct address_space* space, struct memory_region* region);

/* Destroy every region attached to an address space, used during teardown. */
bool memory_region_destroy_all(struct address_space* space);

/* Grow the region metadata table when capacity has been exhausted. */
bool memory_region_ensure_capacity(struct address_space* space);

/* Look up a region by its vmm_id. */
struct memory_region* memory_region_find_by_id(struct address_space* space, vmm_id_t id);

/* Look up a region whose usable base matches base exactly. */
struct memory_region* memory_region_find_by_base(struct address_space* space, uintptr_t base);

/* Look up the region that contains addr, or NULL if no region covers it. */
struct memory_region* memory_region_find_containing(struct address_space* space, uintptr_t addr);

/* Return the region stored at slot index, or NULL when index is out of range. */
struct memory_region* memory_region_at(struct address_space* space, size_t index);

/* Return the total number of region slots the address space currently supports. */
size_t memory_region_capacity(const struct address_space* space);

/* Return the number of region slots currently in use. */
size_t memory_region_count(const struct address_space* space);

/* Return true when the slot currently represents a live region. */
bool memory_region_is_used(const struct memory_region* region);

/* Aggregate the live mapping state of all pages in the region. */
enum vmm_state memory_region_state(const struct memory_region* region);

/* Return true when page_index is a valid stack-fault target (above the guard area). */
bool memory_region_stack_fault_is_valid(const struct memory_region* region, size_t page_index);

/* Populate out_info with a public snapshot of the region. */
void memory_region_fill_info(const struct memory_region* region, struct vmm_info* out_info);
