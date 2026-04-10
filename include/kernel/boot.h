#pragma once

#include <core/cpu.h>
#include <core/mm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Simple framebuffer description copied from the boot protocol when one is available. */
struct kernel_boot_framebuffer {
	void*    address;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint16_t bpp;
};

/* Address-space facts reported by the bootloader for the running kernel image. */
struct kernel_boot_address_space {
	uintptr_t direct_map_offset;
	uintptr_t physical_base;
	uintptr_t virtual_base;
};

/* Entry point type used when asking the boot protocol to start an application processor. */
typedef void (*kernel_boot_cpu_entry_t)(size_t cpu_index, void* arg);

/* Parse bootloader responses and cache the kernel-visible boot information. */
bool kernel_boot_init(void);

/* Return true once the current boot protocol has been recognized and initialized. */
bool kernel_boot_protocol_supported(void);

/* Return the primary framebuffer, if the bootloader exposed one. */
bool kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out);

/* Return the cached physical memory map and optionally its entry count. */
const struct mem_range* kernel_boot_memmap(size_t* out_count);

/* Return the raw kernel command line string, or NULL if none was supplied. */
const char* kernel_boot_cmdline(void);

/* Return the ACPI RSDP physical address when the bootloader reported it. */
bool kernel_boot_rsdp_address(uintptr_t* out_address);

/* Return the bootloader's direct-map and kernel image address-space information. */
bool kernel_boot_address_space_get(struct kernel_boot_address_space* out);

/* Return whether the current architecture/boot path exposes boot-time symmetric multiprocessing startup services. */
bool kernel_boot_cpu_mp_supported(void);

/* Fill init_info with discovered CPUs and the bootstrap processor index, wiring in the supplied bootstrap stack for the
 * BSP. */
bool kernel_boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
                              uintptr_t boot_stack_top, size_t* out_cpu_count, size_t* out_bsp_index);

/* Ask the boot protocol to start one application processor at entry(cpu_index, arg). */
bool kernel_boot_cpu_start(size_t cpu_index, kernel_boot_cpu_entry_t entry, void* arg);
