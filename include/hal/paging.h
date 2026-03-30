#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hal_page_flags {
	HAL_PAGE_WRITE    = 1u << 0,
	HAL_PAGE_EXEC     = 1u << 1,
	HAL_PAGE_GLOBAL   = 1u << 2,
	HAL_PAGE_NO_CACHE = 1u << 3,
};

bool hal_paging_init(void);

bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags);
bool hal_paging_unmap(uintptr_t virt);
bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags);
