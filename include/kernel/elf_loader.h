#pragma once

#include <core/process.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Result codes for ELF-driven process creation. */
enum kernel_elf_load_result {
	KERNEL_ELF_LOAD_OK = 0,
	KERNEL_ELF_LOAD_INVALID_ARGUMENTS,
	KERNEL_ELF_LOAD_BAD_FORMAT,
	KERNEL_ELF_LOAD_UNSUPPORTED,
	KERNEL_ELF_LOAD_NO_MEMORY,
	KERNEL_ELF_LOAD_MAP_FAILED,
	KERNEL_ELF_LOAD_COPY_FAILED,
	KERNEL_ELF_LOAD_START_FAILED,
};

/* Output of a successful kernel_elf_load_process() call. */
struct kernel_elf_process {
	struct process* process;
	uintptr_t       entry;
	uintptr_t       heap_base;
	size_t          heap_page_count;
};

/* Load an ELF boot module and a zero-initialized initial heap into a fresh process. Every byte in page-rounded segment

 * * mappings that does not come from the ELF file is cleared before userspace can observe it. The input module may
 * have
 * arbitrary byte alignment. The caller starts the main userspace thread. */
enum kernel_elf_load_result kernel_elf_load_process(const struct kernel_boot_module* module, const char* name,
                                                    struct kernel_elf_process* out_process);
