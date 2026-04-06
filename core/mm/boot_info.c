#include "core/mm.h"

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
