#include <core/lock.h>
#include <core/math.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uintptr_t       alloc_base;
static uint64_t*       alloc_bitmap;
static uintptr_t       alloc_bitmap_phys;
static size_t          alloc_bitmap_pages;
static size_t          total_pages;
static size_t          free_pages;
static bool            initialized;
static struct spinlock vaddr_lock = SPINLOCK_INIT_CLASS("vaddr_lock", SPINLOCK_ORDER_VADDR, SPINLOCK_FLAG_NONE);

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

bool vaddr_alloc_init(uintptr_t base, size_t page_count) {
	uint64_t  span;
	uint64_t  bitmap_span;
	size_t    words;
	size_t    bitmap_bytes;
	size_t    bitmap_pages;
	uintptr_t bitmap_phys = 0;

	spinlock_lock(&vaddr_lock);
	if (initialized && alloc_bitmap_pages != 0) {
		(void)pmm_free_pages(alloc_bitmap_phys, alloc_bitmap_pages);
	}

	alloc_base         = 0;
	alloc_bitmap       = NULL;
	alloc_bitmap_phys  = 0;
	alloc_bitmap_pages = 0;
	total_pages        = 0;
	free_pages         = 0;
	initialized        = false;

	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if (page_count == 0) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if (mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span)) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if ((uint64_t)base + span <= (uint64_t)base) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	words = bitmap_word_count(page_count);
	if (mul_overflow_size(words, sizeof(uint64_t), &bitmap_bytes)) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	bitmap_pages = (bitmap_bytes + (size_t)PMM_PAGE_SIZE - 1u) / (size_t)PMM_PAGE_SIZE;
	if (bitmap_pages == 0) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if (!pmm_alloc_pages(bitmap_pages, &bitmap_phys)) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if (mul_overflow_u64((uint64_t)bitmap_pages, PMM_PAGE_SIZE, &bitmap_span)) {
		(void)pmm_free_pages(bitmap_phys, bitmap_pages);
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	alloc_base         = base;
	alloc_bitmap_phys  = bitmap_phys;
	alloc_bitmap_pages = bitmap_pages;
	alloc_bitmap       = (uint64_t*)hhdm_phys_to_virt(bitmap_phys);
	memset(alloc_bitmap, 0, (size_t)bitmap_span);

	total_pages = page_count;
	free_pages  = page_count;
	initialized = true;
	spinlock_unlock(&vaddr_lock);
	return true;
}

bool vaddr_alloc_is_initialized(void) {
	return initialized;
}

bool vaddr_alloc_reserve(size_t count, size_t align_pages, uintptr_t* out_base) {
	size_t start_page;

	if (out_base) *out_base = 0;

	if (!initialized || !out_base || count == 0) return false;
	spinlock_lock(&vaddr_lock);
	if (align_pages == 0) align_pages = 1;
	if ((align_pages & (align_pages - 1u)) != 0) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}
	if (count > free_pages) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	for (start_page = 0; start_page + count <= total_pages;) {
		size_t aligned_page = start_page;
		bool   fit          = true;

		if ((aligned_page & (align_pages - 1u)) != 0) {
			aligned_page = (aligned_page + align_pages - 1u) & ~(align_pages - 1u);
			start_page   = aligned_page;
			continue;
		}

		for (size_t page = 0; page < count; page++) {
			if (bitmap_test(alloc_bitmap, aligned_page + page)) {
				start_page = aligned_page + page + 1u;
				fit        = false;
				break;
			}
		}

		if (!fit) continue;

		for (size_t page = 0; page < count; page++) {
			bitmap_set(alloc_bitmap, aligned_page + page);
		}

		free_pages -= count;
		*out_base = alloc_base + aligned_page * (uintptr_t)PMM_PAGE_SIZE;
		spinlock_unlock(&vaddr_lock);
		return true;
	}

	spinlock_unlock(&vaddr_lock);
	return false;
}

bool vaddr_alloc_release(uintptr_t base, size_t count) {
	size_t start_page;
	size_t end_page;

	if (!initialized || count == 0) return false;
	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	spinlock_lock(&vaddr_lock);

	if (base < alloc_base) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	start_page = (size_t)((base - alloc_base) / (uintptr_t)PMM_PAGE_SIZE);
	end_page   = start_page + count;

	if (start_page >= total_pages || end_page > total_pages || end_page < start_page) {
		spinlock_unlock(&vaddr_lock);
		return false;
	}

	for (size_t page = start_page; page < end_page; page++) {
		if (!bitmap_test(alloc_bitmap, page)) {
			spinlock_unlock(&vaddr_lock);
			return false;
		}
	}

	for (size_t page = start_page; page < end_page; page++) {
		bitmap_clear(alloc_bitmap, page);
	}

	free_pages += count;
	spinlock_unlock(&vaddr_lock);
	return true;
}

size_t vaddr_alloc_total_page_count(void) {
	size_t count;

	spinlock_lock(&vaddr_lock);
	count = total_pages;
	spinlock_unlock(&vaddr_lock);
	return count;
}

size_t vaddr_alloc_free_page_count(void) {
	size_t count;

	spinlock_lock(&vaddr_lock);
	count = free_pages;
	spinlock_unlock(&vaddr_lock);
	return count;
}
