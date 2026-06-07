#include <base/math.h>
#include <core/backing_store.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static bool alloc_metadata(size_t bytes, void** out_virt, uintptr_t* out_phys, size_t* out_pages) {
	size_t    pages;
	uintptr_t phys = 0;

	if (out_virt) *out_virt = NULL;
	if (out_phys) *out_phys = 0;
	if (out_pages) *out_pages = 0;
	if (!out_virt || !out_phys || !out_pages || bytes == 0) return false;

	pages = (bytes + (size_t)PMM_PAGE_SIZE - 1u) / (size_t)PMM_PAGE_SIZE;
	if (pages == 0) return false;
	if (!pmm_alloc_pages(pages, &phys)) return false;

	*out_virt  = hhdm_phys_to_virt(phys);
	*out_phys  = phys;
	*out_pages = pages;
	memset(*out_virt, 0, pages * (size_t)PMM_PAGE_SIZE);
	return true;
}

static void free_metadata(uintptr_t phys, size_t pages) {
	if (pages == 0) return;
	(void)pmm_free_pages(phys, pages);
}

void backing_store_init(struct backing_store* store, size_t page_count) {
	if (!store) return;
	*store = (struct backing_store){
		.page_count = page_count,
	};
}

bool backing_store_ensure(struct backing_store* store) {
	size_t bytes;

	if (!store) return false;
	if (store->pages != NULL) return true;
	if (store->page_count == 0) return false;
	if (mul_overflow_size(store->page_count, sizeof(uintptr_t), &bytes)) return false;
	return alloc_metadata(bytes, (void**)&store->pages, &store->pages_phys, &store->metadata_pages);
}

void backing_store_release(struct backing_store* store) {
	if (!store) return;
	if (store->pages != NULL) {
		for (size_t page = 0; page < store->page_count; page++) {
			uintptr_t phys = backing_page_phys(store->pages[page]);

			if (phys != 0) (void)pmm_free_pages(phys, 1);
		}
		free_metadata(store->pages_phys, store->metadata_pages);
	}
	backing_store_init(store, store->page_count);
}

void backing_store_release_metadata(struct backing_store* store) {
	if (!store) return;
	if (store->pages != NULL) free_metadata(store->pages_phys, store->metadata_pages);
	backing_store_init(store, store->page_count);
}

void backing_store_release_if_empty(struct backing_store* store) {
	if (!store || !store->pages || store->mapped_count != 0) return;
	for (size_t page = 0; page < store->page_count; page++) {
		if (backing_page_has_phys(store->pages[page])) return;
	}
	free_metadata(store->pages_phys, store->metadata_pages);
	store->pages          = NULL;
	store->pages_phys     = 0;
	store->metadata_pages = 0;
}

bool backing_store_ensure_page(struct backing_store* store, size_t page_index, uintptr_t* out_phys,
                               bool* out_allocated) {
	uintptr_t entry;
	uintptr_t phys;

	if (out_phys) *out_phys = 0;
	if (out_allocated) *out_allocated = false;
	if (!store || page_index >= store->page_count || !out_phys) return false;
	if (!backing_store_ensure(store)) return false;

	entry = store->pages[page_index];
	phys  = backing_page_phys(entry);
	if (phys == 0) {
		if (!pmm_alloc_pages(1, &phys)) return false;
		store->pages[page_index] = backing_page_make(phys, backing_page_flags(entry));
		if (out_allocated) *out_allocated = true;
	}
	*out_phys = phys;
	return true;
}

uintptr_t backing_store_entry(const struct backing_store* store, size_t page_index) {
	if (!store || !store->pages || page_index >= store->page_count) return 0;
	return store->pages[page_index];
}

void backing_store_set_entry(struct backing_store* store, size_t page_index, uintptr_t entry) {
	if (!store || !store->pages || page_index >= store->page_count) return;
	store->pages[page_index] = entry;
}

size_t backing_store_mapped_count(const struct backing_store* store) {
	return store != NULL ? store->mapped_count : 0;
}

void backing_store_set_mapped_count(struct backing_store* store, size_t count) {
	if (store != NULL) store->mapped_count = count;
}

void backing_store_increment_mapped(struct backing_store* store) {
	if (store != NULL) store->mapped_count++;
}

void backing_store_decrement_mapped(struct backing_store* store) {
	if (store != NULL && store->mapped_count != 0) store->mapped_count--;
}
