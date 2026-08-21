#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Metadata tracking physical-page backing for one allocation. */
struct backing_store {
	uintptr_t* pages;
	uintptr_t  pages_phys;
	size_t     metadata_pages;
	size_t     page_count;
};

/* Return the physical address stored in a backing-store entry. */
static inline uintptr_t backing_page_phys(uintptr_t entry) {
	return entry;
}

/* Return true when the entry holds a non-zero physical address. */
static inline bool backing_page_has_phys(uintptr_t entry) {
	return backing_page_phys(entry) != 0;
}

/* Reset a backing store to a zero-page metadata layout, retaining the requested capacity. */
void backing_store_init(struct backing_store* store, size_t page_count);

/* Allocate direct-map storage for the page metadata on demand. */
bool backing_store_ensure(struct backing_store* store);

/* Release direct-map storage and the metadata pages themselves. */
void backing_store_release(struct backing_store* store);

/* Release just the metadata pages while keeping the per-page entries. */
void backing_store_release_metadata(struct backing_store* store);

/* Release both the entries and the metadata when no live pages remain. */
void backing_store_release_if_empty(struct backing_store* store);

/* Ensure page_index has an allocated backing physical page, allocating one if needed. */
bool backing_store_ensure_page(struct backing_store* store, size_t page_index, uintptr_t* out_phys,
                               bool* out_allocated);

/* Read the raw entry stored for page_index. */
uintptr_t backing_store_entry(const struct backing_store* store, size_t page_index);

/* Overwrite the entry stored for page_index. */
void backing_store_set_entry(struct backing_store* store, size_t page_index, uintptr_t entry);
