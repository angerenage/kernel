#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hal_mem_range_type {
	HAL_MEM_RANGE_USABLE = 0,
	HAL_MEM_RANGE_RESERVED,
	HAL_MEM_RANGE_ACPI_RECLAIMABLE,
	HAL_MEM_RANGE_ACPI_NVS,
	HAL_MEM_RANGE_BAD_MEMORY,
	HAL_MEM_RANGE_BOOTLOADER_RECLAIMABLE,
	HAL_MEM_RANGE_KERNEL_AND_MODULES,
	HAL_MEM_RANGE_FRAMEBUFFER,
	HAL_MEM_RANGE_OTHER,
};

struct hal_mem_range {
	uintptr_t               base;
	size_t                  length;
	enum hal_mem_range_type type;
};

struct hal_mm_boot_info {
	const struct hal_mem_range* ranges;
	size_t                      range_count;
	uintptr_t                   direct_map_offset;
};

bool                        hal_mm_init(const struct hal_mm_boot_info* info);
void*                       hal_mm_early_alloc(size_t size, size_t alignment);
const struct hal_mem_range* hal_mm_boot_ranges(size_t* count);
uintptr_t                   hal_mm_direct_map_offset(void);
uintptr_t                   hal_mm_phys_to_virt(uintptr_t phys_addr);
uintptr_t                   hal_mm_virt_to_phys(uintptr_t virt_addr);
