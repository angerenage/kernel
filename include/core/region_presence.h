#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct region_presence_chunk;

/* Sparse, mapping-owned record of pages with live page-table entries. */
struct region_presence {
	struct region_presence_chunk* chunks;
	size_t                        mapped_count;
	/* Keep common small mappings allocation-free. */
	uint64_t inline_bits;
};

void region_presence_init(struct region_presence* presence);
void region_presence_release(struct region_presence* presence);

/* Ensure presence for page_index can be recorded without a later allocation. */
bool region_presence_prepare(struct region_presence* presence, size_t page_index);
void region_presence_discard_empty(struct region_presence* presence, size_t page_index);

bool   region_presence_is_mapped(const struct region_presence* presence, size_t page_index);
void   region_presence_mark_mapped(struct region_presence* presence, size_t page_index);
void   region_presence_clear_mapped(struct region_presence* presence, size_t page_index);
size_t region_presence_mapped_count(const struct region_presence* presence);
