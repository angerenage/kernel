#pragma once

#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hal_paging_space;

enum hal_page_flags {
	HAL_PAGE_READ   = 1u << 0,
	HAL_PAGE_WRITE  = 1u << 1,
	HAL_PAGE_EXEC   = 1u << 2,
	HAL_PAGE_GLOBAL = 1u << 3,
	HAL_PAGE_USER   = 1u << 4,
};

#define HAL_PAGE_VALID_MASK                                                                                            \
	((uint64_t)(HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_EXEC | HAL_PAGE_GLOBAL | HAL_PAGE_USER))

/*
 * leaf_size_mask uses bit N for a supported 2^N-byte translation leaf.
 * minimum_leaf_size is the minimum address and size granularity accepted by the HAL.
 */
struct hal_paging_info {
	size_t   minimum_leaf_size;
	uint64_t leaf_size_mask;
};

/* Parameters for mapping one contiguous physical extent. */
struct hal_paging_map_request {
	uintptr_t        virtual_address;
	uintptr_t        physical_address;
	size_t           size;
	uint64_t         flags;
	enum memory_type memory_type;
};

/* Parameters for retargeting one translated virtual range to a contiguous physical extent. */
struct hal_paging_remap_request {
	uintptr_t virtual_address;
	uintptr_t physical_address;
	size_t    size;
};

/* Information about the translation containing one virtual address. */
struct hal_paging_translation {
	uintptr_t        physical_address;
	size_t           leaf_size;
	uint64_t         flags;
	enum memory_type memory_type;
};

/* Initialize paging and capture the hardware address space currently used by the kernel. */
bool hal_paging_init(void);

/* Return the translation capabilities of the active architecture backend. */
const struct hal_paging_info* hal_paging_info(void);

/* Return the kernel paging space captured during initialization. */
struct hal_paging_space* hal_paging_kernel_space(void);

/* Return whether the requested protection and memory type can be enforced exactly. */
bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type);

/* Create a process paging space with the required kernel mappings. */
bool hal_paging_space_create(struct hal_paging_space** out_space);

/* Destroy one paging space and release its architecture-owned translation state. */
void hal_paging_space_destroy(struct hal_paging_space* space);

/* Activate one process paging space on the current CPU. */
bool hal_paging_activate(const struct hal_paging_space* space);

/* Map one contiguous physical extent into one contiguous virtual range. */
bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request);

/* Retarget a mapped virtual range while preserving its protection and memory type. */
bool hal_paging_remap(struct hal_paging_space* space, const struct hal_paging_remap_request* request);

/* Remove all translations intersecting exactly the supplied virtual range. */
bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virtual_address, size_t size);

/* Change the protection of exactly the supplied virtual range. */
bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virtual_address, size_t size, uint64_t flags);

/* Query the translation containing one virtual address. */
bool hal_paging_query(const struct hal_paging_space* space, uintptr_t virtual_address,
                      struct hal_paging_translation* out_translation);

/* Make bytes written through a kernel mapping visible to instruction fetch. */
void hal_paging_sync_executable_range(void* address, size_t size);
