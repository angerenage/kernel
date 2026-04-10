#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Thin architecture paging interface used by the core VMM.
 * The contract is page-granular and assumes 4 KiB pages only.
 */

enum hal_page_flags {
	HAL_PAGE_WRITE    = 1u << 0,
	HAL_PAGE_EXEC     = 1u << 1,
	HAL_PAGE_GLOBAL   = 1u << 2,
	HAL_PAGE_NO_CACHE = 1u << 3,
};

/* Validate that the currently active page-table root can be queried and updated through this HAL backend. */
bool hal_paging_init(void);

/* Create a single virtual-to-physical mapping. Both addresses must be page aligned and currently unmapped. */
bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Remove a single mapping from the active address space. The caller owns any backing-page lifetime decisions. */
bool hal_paging_unmap(uintptr_t virt);

/* Resolve an existing mapping, returning the translated physical address and reconstructed HAL flags. */
bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags);
