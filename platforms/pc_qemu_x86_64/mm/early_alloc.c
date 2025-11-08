#include <hal/mm.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 0x1000u
#define HAL_MM_MAX_TRACKED_RANGES 128u
#define HAL_MM_MAX_EARLY_REGIONS 64u

struct early_region {
	uintptr_t base;
	uintptr_t end;
	uintptr_t cursor;
};

static struct hal_mem_range boot_ranges[HAL_MM_MAX_TRACKED_RANGES];
static size_t               boot_range_count;
static struct early_region  early_regions[HAL_MM_MAX_EARLY_REGIONS];
static size_t               early_region_count;
static uintptr_t            direct_map_offset;
static bool                 hal_mm_ready;

static uintptr_t align_up_uintptr(uintptr_t value, size_t alignment) {
	if (alignment == 0) return value;
	uintptr_t mask = (uintptr_t)alignment - 1u;
	return (value + mask) & ~mask;
}

static uintptr_t align_down_uintptr(uintptr_t value, size_t alignment) {
	if (alignment == 0) return value;
	uintptr_t mask = (uintptr_t)alignment - 1u;
	return value & ~mask;
}

static size_t normalize_alignment(size_t alignment) {
	if (alignment < sizeof(void*)) {
		alignment = sizeof(void*);
	}

	if ((alignment & (alignment - 1u)) == 0) {
		return alignment;
	}

	alignment--;
	for (size_t shift = 1u; shift < sizeof(size_t) * 8u; shift <<= 1u) {
		alignment |= alignment >> shift;
	}
	alignment++;
	return alignment;
}

bool hal_mm_init(const struct hal_mm_boot_info* info) {
	if (!info || !info->ranges || info->range_count == 0) {
		return false;
	}

	direct_map_offset  = info->direct_map_offset;
	boot_range_count   = 0;
	early_region_count = 0;

	size_t to_copy = info->range_count;
	if (to_copy > HAL_MM_MAX_TRACKED_RANGES) {
		to_copy = HAL_MM_MAX_TRACKED_RANGES;
	}

	for (size_t i = 0; i < to_copy; i++) {
		boot_ranges[i] = info->ranges[i];
	}
	boot_range_count = to_copy;

	if (boot_range_count < info->range_count) {
		printf("hal_mm: warning: truncated boot memory ranges to %u entries\n", (unsigned)boot_range_count);
	}

	for (size_t i = 0; i < boot_range_count; i++) {
		const struct hal_mem_range* range = &boot_ranges[i];

		if (range->type != HAL_MEM_RANGE_USABLE) continue;
		if (range->length == 0) continue;
		if (range->base > UINTPTR_MAX - range->length) continue;

		uintptr_t region_base = align_up_uintptr(range->base, PAGE_SIZE);
		uintptr_t region_end  = align_down_uintptr(range->base + range->length, PAGE_SIZE);

		if (region_end <= region_base) continue;

		if (early_region_count >= HAL_MM_MAX_EARLY_REGIONS) break;

		early_regions[early_region_count++] = (struct early_region){
			.base   = region_base,
			.end    = region_end,
			.cursor = region_base,
		};
	}

	hal_mm_ready = early_region_count > 0;
	return hal_mm_ready;
}

void* hal_mm_early_alloc(size_t size, size_t alignment) {
	if (!hal_mm_ready || size == 0) return NULL;

	alignment              = normalize_alignment(alignment);
	uintptr_t size_aligned = align_up_uintptr((uintptr_t)size, alignment);

	for (size_t i = 0; i < early_region_count; i++) {
		struct early_region* region = &early_regions[i];
		uintptr_t            cursor = align_up_uintptr(region->cursor, alignment);

		if (cursor > region->end) continue;
		if (region->end - cursor < size_aligned) continue;

		region->cursor = cursor + size_aligned;
		return (void*)(cursor + direct_map_offset);
	}

	return NULL;
}

const struct hal_mem_range* hal_mm_boot_ranges(size_t* count) {
	if (count) *count = boot_range_count;
	return boot_ranges;
}

uintptr_t hal_mm_direct_map_offset(void) {
	return direct_map_offset;
}

uintptr_t hal_mm_phys_to_virt(uintptr_t phys_addr) {
	if (!hal_mm_ready) return 0;
	return phys_addr + direct_map_offset;
}

uintptr_t hal_mm_virt_to_phys(uintptr_t virt_addr) {
	if (!hal_mm_ready) return 0;
	if (virt_addr < direct_map_offset) return 0;
	return virt_addr - direct_map_offset;
}
