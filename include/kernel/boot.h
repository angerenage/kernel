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

/* Boot module loaded alongside the kernel image. */
struct kernel_boot_module {
	const char* path;
	const char* name;
	void*       address;
	size_t      size;
	uint32_t    media_type;
};

/* Entry point type used when asking the boot protocol to start an application processor. */
typedef void (*kernel_boot_cpu_entry_t)(size_t cpu_index, void* arg);

/* Parse and validate all required bootloader responses, then publish the cached boot information atomically.
 * On
 * failure, boot getters continue to report that no initialized boot state is available. */
bool kernel_boot_init(void);

/* Return true once the current boot protocol has been recognized and initialized. */
bool kernel_boot_protocol_supported(void);

/* Return the primary framebuffer, if the bootloader exposed one. */
bool kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out);

/* Return the cached physical memory map and optionally its entry count. */
const struct mem_range* kernel_boot_memmap(size_t* out_count);

/* Return the raw kernel command line string, or NULL if none was supplied. */
const char* kernel_boot_cmdline(void);

/* Return the number of boot modules cached from the bootloader. */
size_t kernel_boot_module_count(void);

/* Return a boot module by stable bootloader order, or NULL if index is out of range. */
const struct kernel_boot_module* kernel_boot_module_at(size_t index);

/* Return the first boot module matching name. The module string is matched first, then the path basename. */
const struct kernel_boot_module* kernel_boot_module_find(const char* name);

/* Return the ACPI RSDP physical address when the bootloader reported it. */
bool kernel_boot_rsdp_address(uintptr_t* out_address);

/* Return the bootloader's direct-map and kernel image address-space information. */
bool kernel_boot_address_space_get(struct kernel_boot_address_space* out);

/* Return whether the current architecture/boot path exposes boot-time symmetric multiprocessing startup services. */
bool kernel_boot_cpu_mp_supported(void);

/* Fill init_info with discovered CPUs and the bootstrap processor index, wiring in the supplied bootstrap stack for
 * the
 * BSP. Firmware topologies with null descriptors or a missing/ambiguous BSP identity are rejected. */
bool kernel_boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
                              uintptr_t boot_stack_top, size_t* out_cpu_count, size_t* out_bsp_index);

/* Ask the boot protocol to start one application processor at entry(cpu_index, arg). */
bool kernel_boot_cpu_start(size_t cpu_index, kernel_boot_cpu_entry_t entry, void* arg);
