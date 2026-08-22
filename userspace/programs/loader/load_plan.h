#pragma once

#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A normalized userspace-loader PT_LOAD memory interval. */
struct loader_elf_segment_layout {
	uint64_t   vaddr;
	uint64_t   memsz;
	vmm_prot_t prot;
};

/* A contiguous userspace-loader Memory capability range. */
struct loader_elf_load_region {
	uint64_t virtual_base;
	size_t   page_count;
	size_t   first_run;
	size_t   run_count;
};

/* A uniform-protection mapping interval within a userspace load region. */
struct loader_elf_load_run {
	size_t     object_page_offset;
	size_t     page_count;
	vmm_prot_t prot;
};

/* The private page layout for a userspace-loaded ELF image. */
struct loader_elf_load_plan {
	struct loader_elf_load_region* regions;
	size_t                         region_count;
	struct loader_elf_load_run*    runs;
	size_t                         run_count;
};

/* The outcome of building a userspace ELF load plan. */
enum loader_elf_plan_result {
	LOADER_ELF_PLAN_OK = 0,
	LOADER_ELF_PLAN_INVALID_ARGUMENT,
	LOADER_ELF_PLAN_BAD_LAYOUT,
	LOADER_ELF_PLAN_NO_MEMORY,
};

/* Build userspace load regions and protection runs. */
enum loader_elf_plan_result loader_elf_plan_create(const struct loader_elf_segment_layout* segments,
                                                   size_t segment_count, struct loader_elf_load_plan* out_plan);

/* Release storage owned by a userspace ELF load plan. */
void loader_elf_plan_deinit(struct loader_elf_load_plan* plan);

/* Return whether an entry lies in a logical executable userspace segment. */
bool loader_elf_entry_is_executable(const struct loader_elf_segment_layout* segments, size_t segment_count,
                                    uint64_t entry);
