#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Normalized physical memory map supplied by the boot environment. */
enum mem_range_type {
	MEM_RANGE_USABLE = 0,
	MEM_RANGE_RESERVED,
	MEM_RANGE_ACPI,
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

struct boot_framebuffer {
	void*    address;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint16_t bpp;
	uint8_t  memory_model;
	uint8_t  red_mask_size;
	uint8_t  red_mask_shift;
	uint8_t  green_mask_size;
	uint8_t  green_mask_shift;
	uint8_t  blue_mask_size;
	uint8_t  blue_mask_shift;
};

struct boot_module {
	const char* path;
	const char* name;
	void*       address;
	size_t      size;
	uint32_t    media_type;
};

struct boot_address_space {
	uintptr_t direct_map_offset;
	uintptr_t kernel_physical_base;
	uintptr_t kernel_virtual_base;
};

/*
 * Passive, immutable facts about this boot.  All pointers refer to memory
 * retained by the boot backend for the lifetime of the kernel.
 */
struct boot_info {
	const struct mem_range*   memory_map;
	size_t                    memory_map_count;
	uintptr_t                 direct_map_offset;
	uintptr_t                 kernel_physical_base;
	uintptr_t                 kernel_virtual_base;
	uintptr_t                 rsdp_address;
	uintptr_t                 dtb_address;
	const struct boot_module* modules;
	size_t                    module_count;
	const char*               command_line;
	struct boot_framebuffer   framebuffer;
	bool                      framebuffer_available;
};

/* Return the published immutable boot snapshot, or NULL before boot_init(). */
const struct boot_info*   boot_info_get(void);
bool                      boot_address_space_get(struct boot_address_space* out);
bool                      boot_framebuffer_get(struct boot_framebuffer* out);
size_t                    boot_module_count(void);
const struct boot_module* boot_module_get(size_t index);
const struct boot_module* boot_module_lookup(const char* name);

const struct boot_module* boot_module_at(const struct boot_info* info, size_t index);
const struct boot_module* boot_module_find(const struct boot_info* info, const char* name);
const char*               boot_cmdline(const struct boot_info* info);
const char*               boot_cmdline_current(void);
const char*               mem_range_type_str(enum mem_range_type type);
