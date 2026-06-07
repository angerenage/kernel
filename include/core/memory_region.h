#pragma once

#include <base/vmm.h>
#include <core/backing_store.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory_region {
	vmm_id_t      id;
	uintptr_t     reserved_base;
	uintptr_t     base;
	size_t        reserved_page_count;
	size_t        page_count;
	vmm_prot_t    prot;
	enum vmm_kind kind;
	size_t        guard_pages;
	uint64_t      map_flags;
	/* When true, the VMM owns the backing physical pages and frees them on destroy.
	 * When false, the physical pages are external and only the mapping is freed. */
	bool                 owns_pages;
	struct backing_store backing;
	bool                 used;
};

struct memory_region_create_result {
	struct memory_region* region;
	uintptr_t             base;
};

bool memory_region_params_allowed(const struct address_space* space, const struct vmm_alloc_params* params);
bool memory_region_create(struct address_space* space, uintptr_t requested_base, const struct vmm_alloc_params* params,
                          struct memory_region_create_result* out_result);
bool memory_region_create_phys(struct address_space* space, uintptr_t requested_base, uintptr_t phys_base,
                               const struct vmm_alloc_params* params, struct memory_region** out_region,
                               uintptr_t* out_base);
bool memory_region_destroy(struct address_space* space, struct memory_region* region);
void memory_region_destroy_all(struct address_space* space);
bool memory_region_ensure_capacity(struct address_space* space);
struct memory_region* memory_region_find_by_id(struct address_space* space, vmm_id_t id);
struct memory_region* memory_region_find_by_base(struct address_space* space, uintptr_t base);
struct memory_region* memory_region_find_containing(struct address_space* space, uintptr_t addr);
struct memory_region* memory_region_at(struct address_space* space, size_t index);
size_t                memory_region_capacity(const struct address_space* space);
size_t                memory_region_count(const struct address_space* space);
bool                  memory_region_is_used(const struct memory_region* region);
enum vmm_state        memory_region_state(const struct memory_region* region);
bool                  memory_region_stack_fault_is_valid(const struct memory_region* region, size_t page_index);
void                  memory_region_fill_info(const struct memory_region* region, struct vmm_info* out_info);
