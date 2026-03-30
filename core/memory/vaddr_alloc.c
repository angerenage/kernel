#include <core/early_alloc.h>
#include <core/pmm.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vaddr_region {
	uintptr_t            base;
	size_t               page_count;
	bool                 free;
	struct vaddr_region* next;
};

static struct vaddr_region* region_list;
static struct vaddr_region* spare_regions;
static size_t               total_pages;
static size_t               free_pages;
static bool                 initialized;

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

	if (!align || (align & (align - 1u)) != 0) return false;
	if (add_overflow_u64(value, align - 1u, &tmp)) return false;

	*out = tmp & ~(align - 1u);
	return true;
}

static struct vaddr_region* alloc_region(void) {
	struct vaddr_region* region;

	if (spare_regions != NULL) {
		region        = spare_regions;
		spare_regions = spare_regions->next;
		region->next  = NULL;
		return region;
	}

	region = early_calloc(1, sizeof(*region), _Alignof(struct vaddr_region));
	return region;
}

static void recycle_region(struct vaddr_region* region) {
	if (!region) return;

	region->next  = spare_regions;
	spare_regions = region;
}

bool vaddr_alloc_init(uintptr_t base, size_t page_count) {
	uint64_t span;

	region_list   = NULL;
	spare_regions = NULL;
	total_pages   = 0;
	free_pages    = 0;
	initialized   = false;

	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (page_count == 0) return false;
	if (mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span)) return false;
	if ((uint64_t)base + span <= (uint64_t)base) return false;

	region_list = alloc_region();
	if (!region_list) return false;

	*region_list = (struct vaddr_region){
		.base       = base,
		.page_count = page_count,
		.free       = true,
		.next       = NULL,
	};

	total_pages = page_count;
	free_pages  = page_count;
	initialized = true;
	return true;
}

bool vaddr_alloc_is_initialized(void) {
	return initialized;
}

bool vaddr_alloc_reserve(size_t count, size_t align_pages, uintptr_t* out_base) {
	struct vaddr_region* region;
	uint64_t             align_bytes;
	uint64_t             aligned_base;

	if (out_base) *out_base = 0;

	if (!initialized || !out_base || count == 0) return false;
	if (align_pages == 0) align_pages = 1;
	if ((align_pages & (align_pages - 1u)) != 0) return false;
	if (count > free_pages) return false;
	if (mul_overflow_u64((uint64_t)align_pages, PMM_PAGE_SIZE, &align_bytes)) return false;

	for (region = region_list; region != NULL; region = region->next) {
		uint64_t             region_end;
		uint64_t             alloc_end;
		uint64_t             prefix_pages64;
		uint64_t             suffix_pages64;
		size_t               prefix_pages;
		size_t               suffix_pages;
		struct vaddr_region* alloc_region_node;
		struct vaddr_region* suffix_region_node;
		struct vaddr_region* next_region;

		if (!region->free || region->page_count < count) continue;
		if (!mul_overflow_u64((uint64_t)region->page_count, PMM_PAGE_SIZE, &region_end)) {
			region_end += (uint64_t)region->base;
		}
		else {
			continue;
		}

		if (!align_up_u64((uint64_t)region->base, align_bytes, &aligned_base)) continue;

		if (aligned_base < (uint64_t)region->base || aligned_base >= region_end) continue;
		if (mul_overflow_u64((uint64_t)count, PMM_PAGE_SIZE, &alloc_end)) continue;
		if (add_overflow_u64(aligned_base, alloc_end, &alloc_end)) continue;
		if (alloc_end > region_end) continue;

		prefix_pages64 = (aligned_base - (uint64_t)region->base) / PMM_PAGE_SIZE;
		suffix_pages64 = (region_end - alloc_end) / PMM_PAGE_SIZE;
		prefix_pages   = (size_t)prefix_pages64;
		suffix_pages   = (size_t)suffix_pages64;

		if (prefix_pages == 0 && suffix_pages == 0) {
			region->free = false;
			free_pages -= count;
			*out_base = region->base;
			return true;
		}

		if (prefix_pages == 0) {
			suffix_region_node = alloc_region();
			if (!suffix_region_node) return false;

			next_region        = region->next;
			region->base       = (uintptr_t)aligned_base;
			region->page_count = count;
			region->free       = false;
			region->next       = suffix_region_node;

			*suffix_region_node = (struct vaddr_region){
				.base       = (uintptr_t)alloc_end,
				.page_count = suffix_pages,
				.free       = true,
				.next       = next_region,
			};

			free_pages -= count;
			*out_base = region->base;
			return true;
		}

		alloc_region_node = alloc_region();
		if (!alloc_region_node) return false;

		suffix_region_node = NULL;
		if (suffix_pages != 0) {
			suffix_region_node = alloc_region();
			if (!suffix_region_node) {
				recycle_region(alloc_region_node);
				return false;
			}
		}

		next_region        = region->next;
		region->page_count = prefix_pages;
		region->next       = alloc_region_node;

		*alloc_region_node = (struct vaddr_region){
			.base       = (uintptr_t)aligned_base,
			.page_count = count,
			.free       = false,
			.next       = next_region,
		};

		if (suffix_region_node != NULL) {
			*suffix_region_node = (struct vaddr_region){
				.base       = (uintptr_t)alloc_end,
				.page_count = suffix_pages,
				.free       = true,
				.next       = next_region,
			};
			alloc_region_node->next = suffix_region_node;
		}

		free_pages -= count;
		*out_base = alloc_region_node->base;
		return true;
	}

	return false;
}

bool vaddr_alloc_release(uintptr_t base, size_t count) {
	struct vaddr_region* region;
	struct vaddr_region* prev = NULL;

	if (!initialized || count == 0) return false;
	if ((base & (PMM_PAGE_SIZE - 1u)) != 0) return false;

	for (region = region_list; region != NULL; prev = region, region = region->next) {
		if (region->base != base) continue;
		if (region->free || region->page_count != count) return false;

		region->free = true;
		free_pages += count;

		if (region->next != NULL && region->next->free) {
			struct vaddr_region* next_region = region->next;

			region->page_count += next_region->page_count;
			region->next = next_region->next;
			recycle_region(next_region);
		}

		if (prev != NULL && prev->free) {
			prev->page_count += region->page_count;
			prev->next = region->next;
			recycle_region(region);
		}

		return true;
	}

	return false;
}

size_t vaddr_alloc_total_page_count(void) {
	return total_pages;
}

size_t vaddr_alloc_free_page_count(void) {
	return free_pages;
}
