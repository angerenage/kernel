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
	HAL_PAGE_USER     = 1u << 4,
};

#define HAL_PAGE_VALID_MASK                                                                                            \
	((uint64_t)(HAL_PAGE_WRITE | HAL_PAGE_EXEC | HAL_PAGE_GLOBAL | HAL_PAGE_NO_CACHE | HAL_PAGE_USER))

/*
 * Architecture-owned hardware address-space handle.
 *
 * Architectures with one root use lower_root_phys. Split-root architectures use
 * lower_root_phys for user/lower mappings and upper_root_phys for kernel/upper
 * mappings. Kernel mappings are inherited when a new space is created.
 */
struct hal_address_space {
	uintptr_t lower_root_phys;
	uintptr_t upper_root_phys;
	uint64_t  flags;
};

/* Validate that the currently active page-table root can be queried and updated through this HAL backend. */
bool hal_paging_init(void);

/* Return the kernel hardware address space captured during hal_paging_init(). */
struct hal_address_space* hal_paging_kernel_space(void);

/* Create a hardware user address space that inherits the kernel mappings needed to run kernel code. */
bool hal_paging_space_create(struct hal_address_space* out_space);

/* Release the root page owned by a hardware address space. Leaf backing pages remain owned by the VMM/PMM caller. */
void hal_paging_space_destroy(struct hal_address_space* space);

/* Switch the current CPU to the supplied hardware address space. */
bool hal_paging_activate(const struct hal_address_space* space);

/* Create a mapping in a hardware address space. Both addresses must be page aligned and currently unmapped. */
bool hal_paging_map(struct hal_address_space* space, uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Remove hardware mappings from a virtual range. */
bool hal_paging_unmap_range(struct hal_address_space* space, uintptr_t virt, size_t page_count);

/* Change the page flags of hardware mappings in a virtual range. */
bool hal_paging_protect_range(struct hal_address_space* space, uintptr_t virt, size_t page_count, uint64_t flags);

/* Resolve an existing mapping, returning the translated physical address and reconstructed HAL flags. */
bool hal_paging_query(const struct hal_address_space* space, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags);

/* Make bytes written through a kernel mapping visible to instruction fetch. */
void hal_paging_sync_executable_range(void* address, size_t size);
