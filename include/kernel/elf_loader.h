#pragma once

#include <core/process.h>
#include <kernel/boot.h>
#include <stdbool.h>
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
};

/* Load an ELF boot module into a fresh process and queue its main userspace thread. */
enum kernel_elf_load_result kernel_elf_load_process(const struct kernel_boot_module* module, const char* name,
                                                    struct kernel_elf_process* out_process);
