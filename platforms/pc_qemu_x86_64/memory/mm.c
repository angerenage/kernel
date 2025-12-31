#include "hal/mm.h"

#include <limine.h>

struct mm_boot_info boot_info;

const char* mem_range_type_str(enum mem_range_type type) {
	switch (type) {
	case MEM_RANGE_USABLE:
		return "usable";
	case MEM_RANGE_RESERVED:
		return "reserved";
	case MEM_RANGE_ACPI_RECLAIMABLE:
		return "acpi_reclaimable";
	case MEM_RANGE_ACPI_NVS:
		return "acpi_nvs";
	case MEM_RANGE_BAD_MEMORY:
		return "bad_memory";
	case MEM_RANGE_BOOTLOADER_RECLAIMABLE:
		return "bootloader_reclaimable";
	case MEM_RANGE_KERNEL_AND_MODULES:
		return "kernel_and_modules";
	case MEM_RANGE_FRAMEBUFFER:
		return "framebuffer";
	case MEM_RANGE_OTHER:
		return "other";
	}
	return "unknown";
}

enum mem_range_type mem_range_type_from_limine(uint64_t type) {
	switch (type) {
	case LIMINE_MEMMAP_USABLE:
		return MEM_RANGE_USABLE;
	case LIMINE_MEMMAP_RESERVED:
		return MEM_RANGE_RESERVED;
	case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
		return MEM_RANGE_ACPI_RECLAIMABLE;
	case LIMINE_MEMMAP_ACPI_NVS:
		return MEM_RANGE_ACPI_NVS;
	case LIMINE_MEMMAP_BAD_MEMORY:
		return MEM_RANGE_BAD_MEMORY;
	case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
		return MEM_RANGE_BOOTLOADER_RECLAIMABLE;
	case LIMINE_MEMMAP_KERNEL_AND_MODULES:
		return MEM_RANGE_KERNEL_AND_MODULES;
	case LIMINE_MEMMAP_FRAMEBUFFER:
		return MEM_RANGE_FRAMEBUFFER;
	default:
		break;
	}

	return MEM_RANGE_OTHER;
}
