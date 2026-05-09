#pragma once

#include <base/vmm.h>
#include <core/memory_region.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stdint.h>

bool region_pager_map_all(struct address_space* space, struct memory_region* region);
bool region_pager_unmap_all(struct address_space* space, struct memory_region* region, bool release_phys);
bool region_pager_protect_all(struct address_space* space, struct memory_region* region, vmm_prot_t new_prot);
bool region_pager_handle_lazy_fault(struct address_space* space, struct memory_region* region, uintptr_t fault_addr);
bool region_pager_unmap_space(struct address_space* space);
