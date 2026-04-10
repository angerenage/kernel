#pragma once

#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Physical page size managed by the physical memory manager and used throughout the kernel. */
#define PMM_PAGE_SIZE 0x1000ull

/*
 * Physical memory manager.
 *
 * The PMM consumes the boot memory map, carves out its own metadata from usable
 * RAM, and then hands out contiguous runs of 4 KiB physical pages.
 */

/* Initialize the allocator from the boot memory map and record the direct-map offset for metadata access. */
bool pmm_init(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset);

/* Allocate count contiguous physical pages and return the base physical address. */
bool pmm_alloc_pages(size_t count, uintptr_t* out_phys);

/* Free a previously allocated contiguous run of count pages starting at phys. */
bool pmm_free_pages(uintptr_t phys, size_t count);

/* Number of usable memory ranges currently managed by the PMM. */
size_t pmm_managed_range_count(void);

/* Total number of pages under PMM management. */
size_t pmm_total_page_count(void);

/* Number of currently free managed pages. */
size_t pmm_free_page_count(void);
