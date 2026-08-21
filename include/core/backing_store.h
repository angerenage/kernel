#pragma once

#include <core/pmm.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKING_STORE_NODE_ENTRY_COUNT ((size_t)PMM_PAGE_SIZE / sizeof(uintptr_t))
#define BACKING_STORE_RADIX_BITS 9u
#define BACKING_STORE_MAX_DEPTH ((sizeof(size_t) * CHAR_BIT + BACKING_STORE_RADIX_BITS - 1u) / BACKING_STORE_RADIX_BITS)

/* Sparse radix-tree backing for an anonymous logical memory object. */
struct backing_store {
	uintptr_t root_phys;
	size_t    page_count;
	size_t    resident_page_count;
	size_t    metadata_page_count;
	size_t    tree_depth;
};

/* Initialize an empty sparse store without allocating metadata. */
void backing_store_init(struct backing_store* store, size_t page_count);

/* Return the materialized physical page for page_index, or false when it is implicit zero. */
bool backing_store_page_phys(const struct backing_store* store, size_t page_index, uintptr_t* out_phys);

/* Materialize page_index transactionally and report whether this call allocated its data page. */
bool backing_store_ensure_page(struct backing_store* store, size_t page_index, uintptr_t* out_phys,
                               bool* out_allocated);

/* Release a materialized page and recursively prune metadata nodes that become empty. */
bool backing_store_release_page(struct backing_store* store, size_t page_index);

/* Release every resident page and metadata node by walking only the allocated radix tree. */
void backing_store_release(struct backing_store* store);
