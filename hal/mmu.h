#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAL_MMU_PAGE_SIZE 0x1000u

enum {
	HAL_MMU_FLAG_WRITE   = 1u << 0,
	HAL_MMU_FLAG_EXECUTE = 1u << 1,
	HAL_MMU_FLAG_USER    = 1u << 2,
	HAL_MMU_FLAG_GLOBAL  = 1u << 3,
};

bool hal_mmu_init(void);
bool hal_mmu_map(uintptr_t virt_addr, uintptr_t phys_addr, size_t size, uint32_t flags);
bool hal_mmu_unmap(uintptr_t virt_addr, size_t size);
void hal_mmu_invalidate(uintptr_t virt_addr);
