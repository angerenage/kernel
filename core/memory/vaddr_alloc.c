#include <base/math.h>
#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct address_space kernel_address_space = {
	.lock = SPINLOCK_INIT_CLASS("kernel_address_space", SPINLOCK_ORDER_VADDR, SPINLOCK_FLAG_NONE),
};

static inline size_t bitmap_word_count(size_t page_count) {
	return (page_count + 63u) / 64u;
}

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static inline bool bitmap_test(const uint64_t* bitmap, size_t bit) {
	return (bitmap[bit / 64u] & (1ull << (bit % 64u))) != 0;
}

static inline void bitmap_set(uint64_t* bitmap, size_t bit) {
	bitmap[bit / 64u] |= 1ull << (bit % 64u);
}

static inline void bitmap_clear(uint64_t* bitmap, size_t bit) {
	bitmap[bit / 64u] &= ~(1ull << (bit % 64u));
}

static void address_space_reset_locked(struct address_space* space) {
	if (space == NULL) return;
	if (space->initialized && space->bitmap_pages != 0) {
		(void)pmm_free_pages(space->bitmap_phys, space->bitmap_pages);
	}

	space->base         = 0;
	space->hal_space    = (struct hal_address_space){0};
	space->bitmap       = NULL;
	space->bitmap_phys  = 0;
	space->bitmap_pages = 0;
	space->total_pages  = 0;
	space->free_pages   = 0;
	space->initialized  = false;
}

bool address_space_init(struct address_space* space, uintptr_t base, size_t page_count) {
	uint64_t  span;
	uint64_t  bitmap_span;
	size_t    words;
	size_t    bitmap_bytes;
	size_t    bitmap_pages;
	uintptr_t bitmap_phys = 0;

	if (space == NULL) return false;
	if (!space->initialized) {
		spinlock_init_class(&space->lock, "address_space", SPINLOCK_ORDER_VADDR, SPINLOCK_FLAG_NONE);
	}

	spinlock_lock(&space->lock);
	address_space_reset_locked(space);
	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if (page_count == 0) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if (mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span)) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if ((uint64_t)base + span <= (uint64_t)base) {
		spinlock_unlock(&space->lock);
		return false;
	}

	words = bitmap_word_count(page_count);
	if (mul_overflow_size(words, sizeof(uint64_t), &bitmap_bytes)) {
		spinlock_unlock(&space->lock);
		return false;
	}

	bitmap_pages = (bitmap_bytes + (size_t)PMM_PAGE_SIZE - 1u) / (size_t)PMM_PAGE_SIZE;
	if (bitmap_pages == 0) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if (!pmm_alloc_pages(bitmap_pages, &bitmap_phys)) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if (mul_overflow_u64((uint64_t)bitmap_pages, PMM_PAGE_SIZE, &bitmap_span)) {
		(void)pmm_free_pages(bitmap_phys, bitmap_pages);
		spinlock_unlock(&space->lock);
		return false;
	}

	space->base         = base;
	space->bitmap_phys  = bitmap_phys;
	space->bitmap_pages = bitmap_pages;
	space->bitmap       = (uint64_t*)hhdm_phys_to_virt(bitmap_phys);
	memset(space->bitmap, 0, (size_t)bitmap_span);

	space->total_pages = page_count;
	space->free_pages  = page_count;
	if (space->next_region_id == 0u) space->next_region_id = 1u;
	space->initialized = true;
	spinlock_unlock(&space->lock);
	return true;
}

void address_space_deinit(struct address_space* space) {
	if (space == NULL || !space->initialized) return;

	spinlock_lock(&space->lock);
	address_space_reset_locked(space);
	spinlock_unlock(&space->lock);
}

bool address_space_is_initialized(const struct address_space* space) {
	return space != NULL && space->initialized;
}

struct hal_address_space* address_space_hal(struct address_space* space) {
	if (space == NULL || !space->initialized) return NULL;
	return &space->hal_space;
}

bool address_space_activate(struct address_space* space) {
	struct hal_address_space* hal_space = address_space_hal(space);

	if (hal_space == NULL) return false;
	return hal_paging_activate(hal_space);
}

bool address_space_reserve(struct address_space* space, size_t count, size_t align_pages, uintptr_t* out_base) {
	size_t start_page;

	if (out_base) *out_base = 0;

	if (space == NULL || !space->initialized || !out_base || count == 0) return false;
	spinlock_lock(&space->lock);
	if (align_pages == 0) align_pages = 1;
	if ((align_pages & (align_pages - 1u)) != 0) {
		spinlock_unlock(&space->lock);
		return false;
	}
	if (count > space->free_pages) {
		spinlock_unlock(&space->lock);
		return false;
	}

	for (start_page = 0; start_page + count <= space->total_pages;) {
		size_t aligned_page = start_page;
		bool   fit          = true;

		if ((aligned_page & (align_pages - 1u)) != 0) {
			aligned_page = (aligned_page + align_pages - 1u) & ~(align_pages - 1u);
			start_page   = aligned_page;
			continue;
		}

		for (size_t page = 0; page < count; page++) {
			if (bitmap_test(space->bitmap, aligned_page + page)) {
				start_page = aligned_page + page + 1u;
				fit        = false;
				break;
			}
		}

		if (!fit) continue;

		for (size_t page = 0; page < count; page++) {
			bitmap_set(space->bitmap, aligned_page + page);
		}

		space->free_pages -= count;
		*out_base = space->base + aligned_page * (uintptr_t)PMM_PAGE_SIZE;
		spinlock_unlock(&space->lock);
		return true;
	}

	spinlock_unlock(&space->lock);
	return false;
}

bool address_space_reserve_at(struct address_space* space, uintptr_t base, size_t count) {
	size_t start_page;
	size_t end_page;

	if (space == NULL || !space->initialized || count == 0) return false;
	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) return false;

	spinlock_lock(&space->lock);
	if (base < space->base) {
		spinlock_unlock(&space->lock);
		return false;
	}

	start_page = (size_t)((base - space->base) / (uintptr_t)PMM_PAGE_SIZE);
	end_page   = start_page + count;
	if (start_page >= space->total_pages || end_page > space->total_pages || end_page < start_page ||
	    count > space->free_pages) {
		spinlock_unlock(&space->lock);
		return false;
	}

	for (size_t page = start_page; page < end_page; page++) {
		if (bitmap_test(space->bitmap, page)) {
			spinlock_unlock(&space->lock);
			return false;
		}
	}

	for (size_t page = start_page; page < end_page; page++) {
		bitmap_set(space->bitmap, page);
	}

	space->free_pages -= count;
	spinlock_unlock(&space->lock);
	return true;
}

bool address_space_release(struct address_space* space, uintptr_t base, size_t count) {
	size_t start_page;
	size_t end_page;

	if (space == NULL || !space->initialized || count == 0) return false;
	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	spinlock_lock(&space->lock);

	if (base < space->base) {
		spinlock_unlock(&space->lock);
		return false;
	}

	start_page = (size_t)((base - space->base) / (uintptr_t)PMM_PAGE_SIZE);
	end_page   = start_page + count;

	if (start_page >= space->total_pages || end_page > space->total_pages || end_page < start_page) {
		spinlock_unlock(&space->lock);
		return false;
	}

	for (size_t page = start_page; page < end_page; page++) {
		if (!bitmap_test(space->bitmap, page)) {
			spinlock_unlock(&space->lock);
			return false;
		}
	}

	for (size_t page = start_page; page < end_page; page++) {
		bitmap_clear(space->bitmap, page);
	}

	space->free_pages += count;
	spinlock_unlock(&space->lock);
	return true;
}

size_t address_space_total_page_count(struct address_space* space) {
	size_t count;

	if (space == NULL || !space->initialized) return 0u;
	spinlock_lock(&space->lock);
	count = space->total_pages;
	spinlock_unlock(&space->lock);
	return count;
}

size_t address_space_free_page_count(struct address_space* space) {
	size_t count;

	if (space == NULL || !space->initialized) return 0u;
	spinlock_lock(&space->lock);
	count = space->free_pages;
	spinlock_unlock(&space->lock);
	return count;
}

struct address_space* address_space_kernel(void) {
	return &kernel_address_space;
}
