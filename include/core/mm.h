#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot-time memory-range classification imported from the boot protocol and reused by the physical allocator. */
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

/* One physical memory extent from the bootloader-provided memory map. */
struct mem_range {
	uintptr_t           base;
	size_t              length;
	enum mem_range_type type;
};

/* Global boot address-space facts needed by allocators that translate physical memory through the direct map. */
struct mm_boot_info {
	uintptr_t direct_map_offset;
};

extern struct mm_boot_info boot_info;

/* Convert a mem_range_type value to a short diagnostic string. */
const char* mem_range_type_str(enum mem_range_type type);
