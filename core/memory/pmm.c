#include <core/early_alloc.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BITS_PER_WORD (sizeof(uint64_t) * 8u)
#define PMM_SIZE_MAX ((size_t)-1)

struct pmm_range {
	uintptr_t base;
	size_t    page_count;
	size_t    free_pages;
	uint64_t* bitmap;
};

static struct pmm_range* ranges              = NULL;
static size_t            managed_range_count = 0;
static size_t            total_page_count    = 0;
static size_t            free_page_count     = 0;
static bool              initialized         = false;

static inline bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
	return __builtin_add_overflow(a, b, out);
#endif
#endif
	*out = a + b;
	return *out < a;
}

static inline bool mul_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
	return __builtin_mul_overflow(a, b, out);
#endif
#endif
	if (a && b > UINT64_MAX / a) return true;
	*out = a * b;
	return false;
}

static inline bool align_up_u64(uint64_t value, uint64_t align, uint64_t* out) {
	uint64_t tmp;

	if (!align || (align & (align - 1u))) return false;
	if (add_overflow_u64(value, align - 1u, &tmp)) return false;

	*out = tmp & ~(align - 1u);
	return true;
}

static inline uint64_t align_down_u64(uint64_t value, uint64_t align) {
	return value & ~(align - 1u);
}

static inline size_t bitmap_word_count(size_t page_count) {
	return (page_count + BITS_PER_WORD - 1u) / BITS_PER_WORD;
}

static inline bool bitmap_test(const uint64_t* bitmap, size_t bit) {
	return (bitmap[bit / BITS_PER_WORD] & (1ull << (bit % BITS_PER_WORD))) != 0;
}

static inline void bitmap_set(uint64_t* bitmap, size_t bit) {
	bitmap[bit / BITS_PER_WORD] |= 1ull << (bit % BITS_PER_WORD);
}

static inline void bitmap_clear(uint64_t* bitmap, size_t bit) {
	bitmap[bit / BITS_PER_WORD] &= ~(1ull << (bit % BITS_PER_WORD));
}

static bool usable_page_range(const struct mem_range* source, uintptr_t* out_base, size_t* out_pages) {
	uint64_t end;
	uint64_t aligned_base;
	uint64_t aligned_end;
	uint64_t page_count64;

	if (!source || !out_base || !out_pages) return false;
	if (source->type != MEM_RANGE_USABLE) return false;
	if (source->length < PMM_PAGE_SIZE) return false;
	if (add_overflow_u64((uint64_t)source->base, (uint64_t)source->length, &end)) return false;
	if (!align_up_u64((uint64_t)source->base, PMM_PAGE_SIZE, &aligned_base)) return false;

	aligned_end = align_down_u64(end, PMM_PAGE_SIZE);
	if (aligned_end <= aligned_base) return false;

	page_count64 = (aligned_end - aligned_base) / PMM_PAGE_SIZE;
	if (page_count64 == 0 || page_count64 > PMM_SIZE_MAX) return false;

	*out_base  = (uintptr_t)aligned_base;
	*out_pages = (size_t)page_count64;
	return true;
}

static void reserve_pages(struct pmm_range* range, uintptr_t reserved_base, uintptr_t reserved_end) {
	uint64_t range_bytes;
	uint64_t range_end;
	uint64_t start;
	uint64_t end;
	size_t   first_page;
	size_t   last_page;

	if (!range || reserved_end <= reserved_base) return;
	if (mul_overflow_u64((uint64_t)range->page_count, PMM_PAGE_SIZE, &range_bytes)) return;
	if (add_overflow_u64((uint64_t)range->base, range_bytes, &range_end)) return;

	if ((uint64_t)reserved_end <= (uint64_t)range->base || (uint64_t)reserved_base >= range_end) return;

	start = (uint64_t)reserved_base > (uint64_t)range->base ? (uint64_t)reserved_base : (uint64_t)range->base;
	end   = (uint64_t)reserved_end < range_end ? (uint64_t)reserved_end : range_end;

	first_page = (size_t)((start - (uint64_t)range->base) / PMM_PAGE_SIZE);
	last_page  = (size_t)(((end - (uint64_t)range->base) + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE);

	if (last_page > range->page_count) last_page = range->page_count;

	for (size_t page = first_page; page < last_page; page++) {
		if (bitmap_test(range->bitmap, page)) continue;

		bitmap_set(range->bitmap, page);
		range->free_pages--;
		free_page_count--;
	}
}

static bool find_contiguous_free(const struct pmm_range* range, size_t count, size_t* out_start_page) {
	size_t run_start = 0;
	size_t run_count = 0;

	if (!range || !out_start_page || count == 0) return false;

	for (size_t page = 0; page < range->page_count; page++) {
		if (bitmap_test(range->bitmap, page)) {
			run_count = 0;
			continue;
		}

		if (run_count == 0) run_start = page;
		run_count++;

		if (run_count == count) {
			*out_start_page = run_start;
			return true;
		}
	}

	return false;
}

bool pmm_init(void) {
	size_t usable_ranges = 0;
	size_t usable_pages  = 0;
	size_t range_index   = 0;

	ranges              = NULL;
	managed_range_count = 0;
	total_page_count    = 0;
	free_page_count     = 0;
	initialized         = false;

	if (!boot_info.ranges || boot_info.range_count == 0) return false;

	for (size_t i = 0; i < boot_info.range_count; i++) {
		uintptr_t base;
		size_t    page_count;

		if (!usable_page_range(&boot_info.ranges[i], &base, &page_count)) continue;
		if (usable_pages > PMM_SIZE_MAX - page_count) return false;

		usable_ranges++;
		usable_pages += page_count;
	}

	if (usable_ranges == 0 || usable_pages == 0) return false;

	ranges = early_calloc(usable_ranges, sizeof(struct pmm_range), _Alignof(struct pmm_range));
	if (!ranges) return false;

	managed_range_count = usable_ranges;
	total_page_count    = usable_pages;
	free_page_count     = usable_pages;

	for (size_t i = 0; i < boot_info.range_count; i++) {
		uintptr_t base;
		size_t    page_count;
		size_t    words;
		uint64_t* bitmap;

		if (!usable_page_range(&boot_info.ranges[i], &base, &page_count)) continue;

		words  = bitmap_word_count(page_count);
		bitmap = early_calloc(words, sizeof(uint64_t), _Alignof(uint64_t));
		if (!bitmap) return false;

		ranges[range_index++] = (struct pmm_range){
			.base       = base,
			.page_count = page_count,
			.free_pages = page_count,
			.bitmap     = bitmap,
		};
	}

	for (size_t i = 0; i < early_reserved_range_count(); i++) {
		struct early_reserved_range reserved;

		if (!early_reserved_range(i, &reserved)) return false;

		for (size_t j = 0; j < managed_range_count; j++) {
			reserve_pages(&ranges[j], reserved.base, reserved.end);
		}
	}

	initialized = true;
	return true;
}

bool pmm_alloc_pages(size_t count, uintptr_t* out_phys) {
	if (out_phys) *out_phys = 0;

	if (!initialized || !out_phys || count == 0) return false;

	for (size_t i = 0; i < managed_range_count; i++) {
		size_t start_page;

		if (ranges[i].free_pages < count) continue;
		if (!find_contiguous_free(&ranges[i], count, &start_page)) continue;

		for (size_t page = 0; page < count; page++) {
			bitmap_set(ranges[i].bitmap, start_page + page);
		}

		ranges[i].free_pages -= count;
		free_page_count -= count;
		*out_phys = ranges[i].base + start_page * (uintptr_t)PMM_PAGE_SIZE;
		return true;
	}

	return false;
}

bool pmm_free_pages(uintptr_t phys, size_t count) {
	uint64_t span;
	uint64_t end;

	if (!initialized || count == 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (mul_overflow_u64((uint64_t)count, PMM_PAGE_SIZE, &span)) return false;
	if (add_overflow_u64((uint64_t)phys, span, &end)) return false;

	for (size_t i = 0; i < managed_range_count; i++) {
		uint64_t range_span;
		uint64_t range_end;
		size_t   first_page;

		if (mul_overflow_u64((uint64_t)ranges[i].page_count, PMM_PAGE_SIZE, &range_span)) return false;
		if (add_overflow_u64((uint64_t)ranges[i].base, range_span, &range_end)) return false;
		if ((uint64_t)phys < (uint64_t)ranges[i].base || end > range_end) continue;

		first_page = (size_t)(((uint64_t)phys - (uint64_t)ranges[i].base) / PMM_PAGE_SIZE);

		for (size_t page = 0; page < count; page++) {
			if (!bitmap_test(ranges[i].bitmap, first_page + page)) return false;
		}

		for (size_t page = 0; page < count; page++) {
			bitmap_clear(ranges[i].bitmap, first_page + page);
		}

		ranges[i].free_pages += count;
		free_page_count += count;
		return true;
	}

	return false;
}

size_t pmm_managed_range_count(void) {
	return managed_range_count;
}

size_t pmm_total_page_count(void) {
	return total_page_count;
}

size_t pmm_free_page_count(void) {
	return free_page_count;
}
