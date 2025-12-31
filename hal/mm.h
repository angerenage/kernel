#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mem_range_type {
	MEM_RANGE_USABLE = 0,
	MEM_RANGE_RESERVED,
	MEM_RANGE_ACPI_RECLAIMABLE,
	MEM_RANGE_ACPI_NVS,
	MEM_RANGE_BAD_MEMORY,
	MEM_RANGE_BOOTLOADER_RECLAIMABLE,
	MEM_RANGE_KERNEL_AND_MODULES,
	MEM_RANGE_FRAMEBUFFER,
	MEM_RANGE_OTHER,
};

struct mem_range {
	uintptr_t           base;
	size_t              length;
	enum mem_range_type type;
};

struct mm_boot_info {
	struct mem_range* ranges;
	size_t            range_count;
	uintptr_t         direct_map_offset;
};

extern struct mm_boot_info boot_info;

const char*         mem_range_type_str(enum mem_range_type type);
enum mem_range_type mem_range_type_from_limine(uint64_t type);
