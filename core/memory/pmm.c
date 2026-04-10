#include <core/lock.h>
#include <core/math.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BITS_PER_WORD (sizeof(uint64_t) * 8u)
#define PMM_SIZE_MAX ((size_t)-1)

struct pmm_range {
	uintptr_t base;
	size_t    page_count;
	size_t    free_pages;
	uint64_t* bitmap;
};

struct pmm_init_range {
	uintptr_t base;
	size_t    page_count;
	size_t    bitmap_words;
};

struct pmm_bootstrap_cursor {
	uintptr_t base;
	uintptr_t end;
};

struct pmm_reserved_span {
	uintptr_t base;
	uintptr_t end;
};

static struct pmm_range* ranges              = NULL;
static size_t            managed_range_count = 0;
static size_t            total_page_count    = 0;
static size_t            free_page_count     = 0;
static bool              initialized         = false;
static struct spinlock   pmm_lock = SPINLOCK_INIT_CLASS("pmm_lock", SPINLOCK_ORDER_PMM, SPINLOCK_FLAG_ALLOW_EXCEPTION);
struct mm_boot_info      boot_info;

const char* mem_range_type_str(enum mem_range_type type) {
	switch (type) {
	case MEM_RANGE_USABLE:
		return "usable";
	case MEM_RANGE_RESERVED:
		return "reserved";
	case MEM_RANGE_ACPI_RECLAIMABLE:
		return "acpi_reclaimable";
	case MEM_RANGE_ACPI_NVS:
		return "acpi_nvs";
	case MEM_RANGE_BAD_MEMORY:
		return "bad_memory";
	case MEM_RANGE_BOOTLOADER_RECLAIMABLE:
		return "bootloader_reclaimable";
	case MEM_RANGE_KERNEL_AND_MODULES:
		return "kernel_and_modules";
	case MEM_RANGE_FRAMEBUFFER:
		return "framebuffer";
	case MEM_RANGE_OTHER:
		return "other";
	}
	return "unknown";
}

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
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
	if (add_overflow_u64(source->base, source->length, &end)) return false;
	if (!align_up_u64(source->base, PMM_PAGE_SIZE, &aligned_base)) return false;

	aligned_end = align_down_u64(end, PMM_PAGE_SIZE);
	if (aligned_end <= aligned_base) return false;

	page_count64 = (aligned_end - aligned_base) / PMM_PAGE_SIZE;
	if (page_count64 == 0 || page_count64 > PMM_SIZE_MAX) return false;

	*out_base  = (uintptr_t)aligned_base;
	*out_pages = (size_t)page_count64;
	return true;
}

static bool bootstrap_alloc(struct pmm_bootstrap_cursor* cursors, size_t cursor_count, size_t size, size_t align,
                            uintptr_t* out_base, struct pmm_reserved_span* out_reserved) {
	uint64_t normalized_align = normalize_align_u64(align, 1u);

	if (!cursors || !out_base || !out_reserved || size == 0 || normalized_align == 0) return false;

	for (size_t i = 0; i < cursor_count; i++) {
		uint64_t aligned_base;
		uint64_t alloc_end;

		if (!align_up_u64((uint64_t)cursors[i].base, normalized_align, &aligned_base)) continue;
		if (aligned_base < (uint64_t)cursors[i].base || aligned_base > (uint64_t)cursors[i].end) continue;
		if (add_overflow_u64(aligned_base, (uint64_t)size, &alloc_end)) continue;
		if (alloc_end > (uint64_t)cursors[i].end) continue;

		*out_base     = (uintptr_t)aligned_base;
		*out_reserved = (struct pmm_reserved_span){
			.base = cursors[i].base,
			.end  = (uintptr_t)alloc_end,
		};
		cursors[i].base = (uintptr_t)alloc_end;
		return true;
	}

	return false;
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

bool pmm_init(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset) {
	size_t                   usable_ranges  = 0;
	size_t                   usable_pages   = 0;
	size_t                   range_index    = 0;
	size_t                   reserved_count = 0;
	size_t                   ranges_bytes;
	uintptr_t                ranges_phys = 0;
	struct pmm_reserved_span ranges_reserved;

	spinlock_lock(&pmm_lock);

	ranges              = NULL;
	managed_range_count = 0;
	total_page_count    = 0;
	free_page_count     = 0;
	initialized         = false;

	if (memory_map == NULL || range_count == 0u) {
		spinlock_unlock(&pmm_lock);
		return false;
	}

	boot_info.direct_map_offset = direct_map_offset;

	for (size_t i = 0; i < range_count; i++) {
		uintptr_t base;
		size_t    page_count;

		if (!usable_page_range(&memory_map[i], &base, &page_count)) continue;
		if (usable_pages > PMM_SIZE_MAX - page_count) {
			spinlock_unlock(&pmm_lock);
			return false;
		}

		usable_ranges++;
		usable_pages += page_count;
	}

	if (usable_ranges == 0 || usable_pages == 0) {
		spinlock_unlock(&pmm_lock);
		return false;
	}

	struct pmm_init_range       init_ranges[usable_ranges];
	struct pmm_bootstrap_cursor bootstrap_cursors[usable_ranges];
	struct pmm_reserved_span    reserved_spans[usable_ranges + 1u];

	for (size_t i = 0; i < range_count; i++) {
		uintptr_t base;
		size_t    page_count;
		uint64_t  span;

		if (!usable_page_range(&memory_map[i], &base, &page_count)) continue;
		if (mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span)) {
			spinlock_unlock(&pmm_lock);
			return false;
		}

		init_ranges[range_index] = (struct pmm_init_range){
			.base         = base,
			.page_count   = page_count,
			.bitmap_words = bitmap_word_count(page_count),
		};
		bootstrap_cursors[range_index] = (struct pmm_bootstrap_cursor){
			.base = base,
			.end  = base + (uintptr_t)span,
		};
		range_index++;
	}

	if (mul_overflow_size(usable_ranges, sizeof(struct pmm_range), &ranges_bytes)) {
		spinlock_unlock(&pmm_lock);
		return false;
	}
	if (!bootstrap_alloc(bootstrap_cursors,
	                     usable_ranges,
	                     ranges_bytes,
	                     _Alignof(struct pmm_range),
	                     &ranges_phys,
	                     &ranges_reserved)) {
		spinlock_unlock(&pmm_lock);
		return false;
	}

	ranges = (struct pmm_range*)hhdm_phys_to_virt(ranges_phys);
	memset(ranges, 0, ranges_bytes);
	reserved_spans[reserved_count++] = ranges_reserved;

	managed_range_count = usable_ranges;
	total_page_count    = usable_pages;
	free_page_count     = usable_pages;

	for (size_t i = 0; i < usable_ranges; i++) {
		size_t                   bitmap_bytes;
		uintptr_t                bitmap_phys = 0;
		struct pmm_reserved_span bitmap_reserved;
		uint64_t*                bitmap;

		if (mul_overflow_size(init_ranges[i].bitmap_words, sizeof(uint64_t), &bitmap_bytes)) {
			spinlock_unlock(&pmm_lock);
			return false;
		}
		if (!bootstrap_alloc(
				bootstrap_cursors, usable_ranges, bitmap_bytes, _Alignof(uint64_t), &bitmap_phys, &bitmap_reserved)) {
			spinlock_unlock(&pmm_lock);
			return false;
		}

		bitmap = (uint64_t*)hhdm_phys_to_virt(bitmap_phys);
		memset(bitmap, 0, bitmap_bytes);

		ranges[i] = (struct pmm_range){
			.base       = init_ranges[i].base,
			.page_count = init_ranges[i].page_count,
			.free_pages = init_ranges[i].page_count,
			.bitmap     = bitmap,
		};
		reserved_spans[reserved_count++] = bitmap_reserved;
	}

	for (size_t i = 0; i < reserved_count; i++) {
		for (size_t j = 0; j < managed_range_count; j++) {
			reserve_pages(&ranges[j], reserved_spans[i].base, reserved_spans[i].end);
		}
	}

	initialized = true;
	spinlock_unlock(&pmm_lock);
	return true;
}

bool pmm_alloc_pages(size_t count, uintptr_t* out_phys) {
	if (out_phys) *out_phys = 0;

	if (!initialized || !out_phys || count == 0) return false;
	spinlock_lock(&pmm_lock);

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
		spinlock_unlock(&pmm_lock);
		return true;
	}

	spinlock_unlock(&pmm_lock);
	return false;
}

bool pmm_free_pages(uintptr_t phys, size_t count) {
	uint64_t span;
	uint64_t end;

	if (!initialized || count == 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (mul_overflow_u64((uint64_t)count, PMM_PAGE_SIZE, &span)) return false;
	if (add_overflow_u64((uint64_t)phys, span, &end)) return false;
	spinlock_lock(&pmm_lock);

	for (size_t i = 0; i < managed_range_count; i++) {
		uint64_t range_span;
		uint64_t range_end;
		size_t   first_page;

		if (mul_overflow_u64((uint64_t)ranges[i].page_count, PMM_PAGE_SIZE, &range_span)) {
			spinlock_unlock(&pmm_lock);
			return false;
		}
		if (add_overflow_u64((uint64_t)ranges[i].base, range_span, &range_end)) {
			spinlock_unlock(&pmm_lock);
			return false;
		}
		if ((uint64_t)phys < (uint64_t)ranges[i].base || end > range_end) continue;

		first_page = (size_t)(((uint64_t)phys - (uint64_t)ranges[i].base) / PMM_PAGE_SIZE);

		for (size_t page = 0; page < count; page++) {
			if (!bitmap_test(ranges[i].bitmap, first_page + page)) {
				spinlock_unlock(&pmm_lock);
				return false;
			}
		}

		for (size_t page = 0; page < count; page++) {
			bitmap_clear(ranges[i].bitmap, first_page + page);
		}

		ranges[i].free_pages += count;
		free_page_count += count;
		spinlock_unlock(&pmm_lock);
		return true;
	}

	spinlock_unlock(&pmm_lock);
	return false;
}

size_t pmm_managed_range_count(void) {
	size_t count;

	spinlock_lock(&pmm_lock);
	count = managed_range_count;
	spinlock_unlock(&pmm_lock);
	return count;
}

size_t pmm_total_page_count(void) {
	size_t count;

	spinlock_lock(&pmm_lock);
	count = total_page_count;
	spinlock_unlock(&pmm_lock);
	return count;
}

size_t pmm_free_page_count(void) {
	size_t count;

	spinlock_lock(&pmm_lock);
	count = free_page_count;
	spinlock_unlock(&pmm_lock);
	return count;
}
