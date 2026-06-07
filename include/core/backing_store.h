#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKING_PAGE_MAPPED (uintptr_t)1u
#define BACKING_PAGE_ROLLBACK_KEEP (uintptr_t)2u
#define BACKING_PAGE_ROLLBACK_SKIP (uintptr_t)4u

struct backing_store {
	uintptr_t* pages;
	uintptr_t  pages_phys;
	size_t     metadata_pages;
	size_t     page_count;
	size_t     mapped_count;
};

static inline uintptr_t backing_page_phys(uintptr_t entry) {
	return entry & ~(uintptr_t)0xfffull;
}

static inline uintptr_t backing_page_flags(uintptr_t entry) {
	return entry & (uintptr_t)0xfffull;
}

static inline bool backing_page_has_phys(uintptr_t entry) {
	return backing_page_phys(entry) != 0;
}

static inline bool backing_page_is_mapped(uintptr_t entry) {
	return (backing_page_flags(entry) & BACKING_PAGE_MAPPED) != 0;
}

static inline uintptr_t backing_page_make(uintptr_t phys, uintptr_t flags) {
	return backing_page_phys(phys) | flags;
}

void      backing_store_init(struct backing_store* store, size_t page_count);
bool      backing_store_ensure(struct backing_store* store);
void      backing_store_release(struct backing_store* store);
void      backing_store_release_metadata(struct backing_store* store);
void      backing_store_release_if_empty(struct backing_store* store);
bool      backing_store_ensure_page(struct backing_store* store, size_t page_index, uintptr_t* out_phys,
                                    bool* out_allocated);
uintptr_t backing_store_entry(const struct backing_store* store, size_t page_index);
void      backing_store_set_entry(struct backing_store* store, size_t page_index, uintptr_t entry);
size_t    backing_store_mapped_count(const struct backing_store* store);
void      backing_store_set_mapped_count(struct backing_store* store, size_t count);
void      backing_store_increment_mapped(struct backing_store* store);
void      backing_store_decrement_mapped(struct backing_store* store);
