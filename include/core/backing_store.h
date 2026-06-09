#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-page flag bit: backing physical page is currently mapped into the page table. */
#define BACKING_PAGE_MAPPED (uintptr_t)1u
/* Rollback policy bit: keep the backing physical page when tearing down a failed operation. */
#define BACKING_PAGE_ROLLBACK_KEEP (uintptr_t)2u
/* Rollback policy bit: drop the backing physical page when tearing down a failed operation. */
#define BACKING_PAGE_ROLLBACK_SKIP (uintptr_t)4u

/* Metadata tracking physical-page backing and mapping state for one allocation. */
struct backing_store {
	uintptr_t* pages;
	uintptr_t  pages_phys;
	size_t     metadata_pages;
	size_t     page_count;
	size_t     mapped_count;
};

/* Return the physical address stored in a backing-store entry. */
static inline uintptr_t backing_page_phys(uintptr_t entry) {
	return entry & ~(uintptr_t)0xfffull;
}

/* Return the mapping/rollback flags of a backing-store entry. */
static inline uintptr_t backing_page_flags(uintptr_t entry) {
	return entry & (uintptr_t)0xfffull;
}

/* Return true when the entry holds a non-zero physical address. */
static inline bool backing_page_has_phys(uintptr_t entry) {
	return backing_page_phys(entry) != 0;
}

/* Return true when the entry currently marks its page as live in a page table. */
static inline bool backing_page_is_mapped(uintptr_t entry) {
	return (backing_page_flags(entry) & BACKING_PAGE_MAPPED) != 0;
}

/* Pack a physical address and the supplied low-flag bits into a backing-store entry. */
static inline uintptr_t backing_page_make(uintptr_t phys, uintptr_t flags) {
	return backing_page_phys(phys) | flags;
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

/* Return the cached number of pages currently marked mapped. */
size_t backing_store_mapped_count(const struct backing_store* store);

/* Set the cached number of mapped pages directly. */
void backing_store_set_mapped_count(struct backing_store* store, size_t count);

/* Increment the cached count of mapped pages. */
void backing_store_increment_mapped(struct backing_store* store);

/* Decrement the cached count of mapped pages. */
void backing_store_decrement_mapped(struct backing_store* store);
