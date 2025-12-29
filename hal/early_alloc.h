#pragma once

#include <limine.h>
#include <stddef.h>

#include "mm.h"

struct mem_range {
	uintptr_t           base;
	size_t              length;
	enum mem_range_type type;
};

struct mm_boot_info {
	const struct mem_range* ranges;
	size_t                  range_count;
	uintptr_t               direct_map_offset;
};

bool early_init(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset);
