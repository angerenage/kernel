#pragma once

#include <base/vmm.h>
#include <core/memory_region.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stdint.h>

/* Materialize every page in region by allocating and mapping backing physical pages. */
bool region_pager_map_all(struct address_space* space, struct memory_region* region);

/* Remove every live mapping in region, optionally releasing the backing physical pages. */
bool region_pager_unmap_all(struct address_space* space, struct memory_region* region, bool release_phys);

/* Update protection flags across all live pages in region. */
bool region_pager_protect_all(struct address_space* space, struct memory_region* region, vmm_prot_t new_prot);

/* Materialize a single page on demand when the fault is inside a lazy region. */
bool region_pager_handle_lazy_fault(struct address_space* space, struct memory_region* region, uintptr_t fault_addr);

/* Unmap every live mapping owned by the address space during teardown. */
bool region_pager_unmap_space(struct address_space* space);
