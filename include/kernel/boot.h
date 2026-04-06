#pragma once

#include <core/cpu.h>
#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct kernel_boot_framebuffer {
	void*    address;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint16_t bpp;
};

struct kernel_boot_address_space {
	uintptr_t direct_map_offset;
	uintptr_t physical_base;
	uintptr_t virtual_base;
};

typedef void (*kernel_boot_cpu_entry_t)(size_t cpu_index, void* arg);

bool                    kernel_boot_init(void);
bool                    kernel_boot_protocol_supported(void);
bool                    kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out);
const struct mem_range* kernel_boot_memmap(size_t* out_count);
const char*             kernel_boot_cmdline(void);
bool                    kernel_boot_rsdp_address(uintptr_t* out_address);
bool                    kernel_boot_address_space_get(struct kernel_boot_address_space* out);
bool                    kernel_boot_cpu_mp_supported(void);
bool kernel_boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
                              uintptr_t boot_stack_top, size_t* out_cpu_count, size_t* out_bsp_index);
bool kernel_boot_cpu_start(size_t cpu_index, kernel_boot_cpu_entry_t entry, void* arg);
