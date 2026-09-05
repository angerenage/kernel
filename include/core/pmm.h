#pragma once

#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pmm_claim_result {
	PMM_CLAIM_OK = 0,
	PMM_CLAIM_NOT_MANAGED,
	PMM_CLAIM_UNAVAILABLE,
};

struct pmm_info {
	size_t allocation_granule;
};

struct pmm_extent {
	uintptr_t address;
	size_t    size;
};

struct pmm_alloc_request {
	size_t    size;            /* Nonzero multiple of allocation_granule. */
	size_t    alignment;       /* Zero selects allocation_granule, otherwise a power of two >= allocation_granule. */
	uintptr_t minimum_address; /* Inclusive. */
	uintptr_t maximum_address; /* Exclusive; zero means unbounded. */
};

/* Initialize physical memory management from the boot memory map. */
bool pmm_init(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset);

/* Return physical allocation properties. */
const struct pmm_info* pmm_info(void);

/* Allocate one contiguous physical extent. out_extent is defined only on success. */
bool pmm_alloc(const struct pmm_alloc_request* request, struct pmm_extent* out_extent);

/* Claim one exact free extent from PMM-managed memory. */
enum pmm_claim_result pmm_claim(struct pmm_extent extent);

/* Release one allocated or claimed physical extent. */
bool pmm_free(struct pmm_extent extent);

/* Return the number of physical memory ranges managed by the PMM. */
size_t pmm_managed_range_count(void);

/* Return the total amount of managed physical memory. */
size_t pmm_total_size(void);

/* Return the currently available managed physical memory. */
size_t pmm_free_size(void);
