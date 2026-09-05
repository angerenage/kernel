#include <base/math.h>
#include <core/lock.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PMM_ALLOCATION_GRANULE 4096u
#define PMM_BITS_PER_WORD (sizeof(uint64_t) * CHAR_BIT)

_Static_assert((PMM_ALLOCATION_GRANULE & (PMM_ALLOCATION_GRANULE - 1u)) == 0u,
               "PMM allocation granule must be a power of two");

struct pmm_range {
	uintptr_t address;
	size_t    size;
	size_t    granule_count;
	size_t    free_granules;
	size_t    first_free_granule;
	uint64_t* allocated;
	uint64_t* reserved;
};

struct pmm_normalized_range {
	uintptr_t address;
	size_t    size;
};

static const struct pmm_info pmm_properties = {.allocation_granule = PMM_ALLOCATION_GRANULE};
static struct pmm_range*     pmm_ranges;
static size_t                pmm_range_count;
static size_t                pmm_total_bytes;
static size_t                pmm_free_bytes;
static bool                  pmm_initialized;
static struct spinlock pmm_lock = SPINLOCK_INIT_CLASS("pmm_lock", SPINLOCK_ORDER_PMM, SPINLOCK_FLAG_ALLOW_EXCEPTION);

struct mm_boot_info boot_info;

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

static inline void* pmm_phys_to_virt(uintptr_t address, uintptr_t direct_map_offset) {
	return (void*)(address + direct_map_offset);
}

static inline size_t pmm_bitmap_words(size_t granule_count) {
	return granule_count / PMM_BITS_PER_WORD + (granule_count % PMM_BITS_PER_WORD != 0u);
}

static inline bool pmm_bitmap_test(const uint64_t* bitmap, size_t bit) {
	return (bitmap[bit / PMM_BITS_PER_WORD] & (1ull << (bit % PMM_BITS_PER_WORD))) != 0u;
}

static inline void pmm_bitmap_set(uint64_t* bitmap, size_t bit) {
	bitmap[bit / PMM_BITS_PER_WORD] |= 1ull << (bit % PMM_BITS_PER_WORD);
}

static inline void pmm_bitmap_clear(uint64_t* bitmap, size_t bit) {
	bitmap[bit / PMM_BITS_PER_WORD] &= ~(1ull << (bit % PMM_BITS_PER_WORD));
}

static bool pmm_memory_map_valid(const struct mem_range* memory_map, size_t range_count) {
	if (memory_map == NULL || range_count == 0u) return false;
	for (size_t i = 0u; i < range_count; i++) {
		uint64_t left_end;

		if (memory_map[i].length == 0u) continue;
		if (add_overflow_u64((uint64_t)memory_map[i].base, (uint64_t)memory_map[i].length, &left_end)) return false;
		for (size_t j = i + 1u; j < range_count; j++) {
			uint64_t right_end;

			if (memory_map[j].length == 0u) continue;
			if (add_overflow_u64((uint64_t)memory_map[j].base, (uint64_t)memory_map[j].length, &right_end))
				return false;
			if ((uint64_t)memory_map[i].base < right_end && (uint64_t)memory_map[j].base < left_end) return false;
		}
	}
	return true;
}

static bool pmm_usable_extent(const struct mem_range* source, struct pmm_normalized_range* out) {
	uint64_t raw_end;
	uint64_t address;
	uint64_t end;

	if (source == NULL || out == NULL || source->type != MEM_RANGE_USABLE || source->length == 0u ||
	    add_overflow_u64((uint64_t)source->base, (uint64_t)source->length, &raw_end) ||
	    !align_up_u64((uint64_t)source->base, PMM_ALLOCATION_GRANULE, &address))
		return false;
	end = align_down_u64(raw_end, PMM_ALLOCATION_GRANULE);
	if (end <= address || end - address > SIZE_MAX) return false;
	*out = (struct pmm_normalized_range){.address = (uintptr_t)address, .size = (size_t)(end - address)};
	return true;
}

/* Return the lowest normalized usable extent beginning at or after minimum,
 * merging all physically adjacent usable map entries. */
static bool pmm_next_range(const struct mem_range* memory_map, size_t range_count, uintptr_t minimum,
                           struct pmm_normalized_range* out) {
	struct pmm_normalized_range result = {0};
	bool                        found  = false;

	for (size_t i = 0u; i < range_count; i++) {
		struct pmm_normalized_range candidate;

		if (!pmm_usable_extent(&memory_map[i], &candidate) || candidate.address < minimum) continue;
		if (!found || candidate.address < result.address) {
			result = candidate;
			found  = true;
		}
	}
	if (!found) return false;

	for (;;) {
		uintptr_t end  = result.address + result.size;
		bool      grew = false;

		for (size_t i = 0u; i < range_count; i++) {
			struct pmm_normalized_range candidate;

			if (!pmm_usable_extent(&memory_map[i], &candidate) || candidate.address != end) continue;
			if (candidate.size > SIZE_MAX - result.size) return false;
			result.size += candidate.size;
			grew = true;
			break;
		}
		if (!grew) break;
	}
	*out = result;
	return true;
}

static bool pmm_measure_ranges(const struct mem_range* memory_map, size_t range_count, size_t* out_count,
                               size_t* out_total_size, size_t* out_bitmap_bytes) {
	uintptr_t minimum = 0u;
	size_t    count = 0u, total_size = 0u, bitmap_bytes = 0u;

	for (;;) {
		struct pmm_normalized_range range;
		size_t                      bytes;

		if (!pmm_next_range(memory_map, range_count, minimum, &range)) break;
		if (mul_overflow_size(pmm_bitmap_words(range.size / PMM_ALLOCATION_GRANULE), sizeof(uint64_t), &bytes) ||
		    total_size > SIZE_MAX - range.size || bitmap_bytes > SIZE_MAX - bytes || count == SIZE_MAX)
			return false;
		count++;
		total_size += range.size;
		bitmap_bytes += bytes;
		if (range.address > UINTPTR_MAX - range.size) return false;
		minimum = range.address + range.size;
		if (minimum == 0u) break;
	}
	if (count == 0u || total_size == 0u) return false;
	*out_count        = count;
	*out_total_size   = total_size;
	*out_bitmap_bytes = bitmap_bytes;
	return true;
}

static bool pmm_metadata_layout(size_t range_count, size_t bitmap_bytes, size_t* out_descriptor_bytes,
                                size_t* out_metadata_size) {
	size_t   descriptor_bytes;
	size_t   metadata_bytes;
	uint64_t aligned;

	if (mul_overflow_size(range_count, sizeof(struct pmm_range), &descriptor_bytes) ||
	    add_overflow_size(descriptor_bytes, bitmap_bytes, &metadata_bytes) ||
	    add_overflow_size(metadata_bytes, bitmap_bytes, &metadata_bytes) ||
	    !align_up_u64(metadata_bytes, PMM_ALLOCATION_GRANULE, &aligned) || aligned > SIZE_MAX)
		return false;
	*out_descriptor_bytes = descriptor_bytes;
	*out_metadata_size    = (size_t)aligned;
	return true;
}

static bool pmm_find_metadata_extent(const struct mem_range* memory_map, size_t range_count, size_t metadata_size,
                                     uintptr_t* out_address) {
	uintptr_t minimum = 0u;

	for (;;) {
		struct pmm_normalized_range range;

		if (!pmm_next_range(memory_map, range_count, minimum, &range)) return false;
		if (range.size >= metadata_size) {
			*out_address = range.address;
			return true;
		}
		minimum = range.address + range.size;
		if (minimum == 0u) return false;
	}
}

static struct pmm_range* pmm_find_containing_range(uintptr_t address, size_t size, bool* intersects) {
	uint64_t end;

	if (intersects != NULL) *intersects = false;
	if (add_overflow_u64((uint64_t)address, (uint64_t)size, &end)) return NULL;
	for (size_t i = 0u; i < pmm_range_count; i++) {
		uint64_t range_end = (uint64_t)pmm_ranges[i].address + pmm_ranges[i].size;

		if (end <= pmm_ranges[i].address || address >= range_end) continue;
		if (intersects != NULL) *intersects = true;
		if (address >= pmm_ranges[i].address && end <= range_end) return &pmm_ranges[i];
		return NULL;
	}
	return NULL;
}

static bool pmm_extent_valid(struct pmm_extent extent) {
	return extent.size != 0u && (extent.address & (PMM_ALLOCATION_GRANULE - 1u)) == 0u &&
	       (extent.size & (PMM_ALLOCATION_GRANULE - 1u)) == 0u && extent.size <= UINTPTR_MAX - extent.address;
}

static bool pmm_find_free_extent(const struct pmm_range* range, const struct pmm_alloc_request* request,
                                 size_t* out_first_granule) {
	uintptr_t range_end = range->address + range->size;
	uintptr_t lower     = request->minimum_address > range->address ? request->minimum_address : range->address;
	uintptr_t upper =
		request->maximum_address != 0u && request->maximum_address < range_end ? request->maximum_address : range_end;
	uint64_t candidate;

	if (range->first_free_granule == range->granule_count) return false;
	uintptr_t first_free_address = range->address + range->first_free_granule * PMM_ALLOCATION_GRANULE;
	if (lower < first_free_address) lower = first_free_address;
	if (!align_up_u64(lower, request->alignment, &candidate)) return false;
	while (candidate < upper && request->size <= upper - candidate) {
		size_t first = (size_t)((candidate - range->address) / PMM_ALLOCATION_GRANULE);
		size_t count = request->size / PMM_ALLOCATION_GRANULE;
		size_t used  = count;

		for (size_t i = 0u; i < count; i++) {
			if (!pmm_bitmap_test(range->allocated, first + i)) continue;
			used = i;
			break;
		}
		if (used == count) {
			*out_first_granule = first;
			return true;
		}
		if (!align_up_u64(candidate + (used + 1u) * PMM_ALLOCATION_GRANULE, request->alignment, &candidate))
			return false;
	}
	return false;
}

bool pmm_init(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset) {
	size_t            normalized_count;
	size_t            total_size;
	size_t            bitmap_bytes;
	size_t            descriptor_bytes;
	size_t            metadata_size;
	uintptr_t         metadata_address;
	struct pmm_range* new_ranges;
	uint8_t*          bitmap_cursor;
	uint8_t*          reserved_cursor;
	uintptr_t         minimum            = 0u;
	size_t            initialized_ranges = 0u;
	struct pmm_range* metadata_range;
	struct irq_state  state = spinlock_lock_irqsave(&pmm_lock);

	pmm_initialized = false;
	pmm_ranges      = NULL;
	pmm_range_count = 0u;
	pmm_total_bytes = 0u;
	pmm_free_bytes  = 0u;

	if (!pmm_memory_map_valid(memory_map, range_count) ||
	    !pmm_measure_ranges(memory_map, range_count, &normalized_count, &total_size, &bitmap_bytes) ||
	    !pmm_metadata_layout(normalized_count, bitmap_bytes, &descriptor_bytes, &metadata_size) ||
	    metadata_size > total_size ||
	    !pmm_find_metadata_extent(memory_map, range_count, metadata_size, &metadata_address)) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}

	new_ranges      = pmm_phys_to_virt(metadata_address, direct_map_offset);
	bitmap_cursor   = (uint8_t*)new_ranges + descriptor_bytes;
	reserved_cursor = bitmap_cursor + bitmap_bytes;
	memset(new_ranges, 0, metadata_size);

	for (;;) {
		struct pmm_normalized_range normalized;
		size_t                      granules;
		size_t                      bytes;

		if (!pmm_next_range(memory_map, range_count, minimum, &normalized)) break;
		granules                         = normalized.size / PMM_ALLOCATION_GRANULE;
		bytes                            = pmm_bitmap_words(granules) * sizeof(uint64_t);
		new_ranges[initialized_ranges++] = (struct pmm_range){
			.address            = normalized.address,
			.size               = normalized.size,
			.granule_count      = granules,
			.free_granules      = granules,
			.first_free_granule = 0u,
			.allocated          = (uint64_t*)bitmap_cursor,
			.reserved           = (uint64_t*)reserved_cursor,
		};
		bitmap_cursor += bytes;
		reserved_cursor += bytes;
		minimum = normalized.address + normalized.size;
		if (minimum == 0u) break;
	}
	if (initialized_ranges != normalized_count) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}

	pmm_ranges      = new_ranges;
	pmm_range_count = normalized_count;
	metadata_range  = pmm_find_containing_range(metadata_address, metadata_size, NULL);
	if (metadata_range == NULL) {
		pmm_ranges      = NULL;
		pmm_range_count = 0u;
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}
	{
		size_t first = (metadata_address - metadata_range->address) / PMM_ALLOCATION_GRANULE;
		size_t count = metadata_size / PMM_ALLOCATION_GRANULE;

		for (size_t i = 0u; i < count; i++) {
			pmm_bitmap_set(metadata_range->allocated, first + i);
			pmm_bitmap_set(metadata_range->reserved, first + i);
		}
		metadata_range->free_granules -= count;
		metadata_range->first_free_granule = first + count;
	}

	boot_info.direct_map_offset = direct_map_offset;
	pmm_total_bytes             = total_size;
	pmm_free_bytes              = total_size - metadata_size;
	pmm_initialized             = true;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return true;
}

const struct pmm_info* pmm_info(void) {
	return &pmm_properties;
}

bool pmm_alloc(const struct pmm_alloc_request* request, struct pmm_extent* out_extent) {
	struct pmm_alloc_request normalized;
	struct irq_state         state;

	if (out_extent != NULL) *out_extent = (struct pmm_extent){0};
	if (request == NULL || out_extent == NULL) return false;
	normalized = *request;
	if (normalized.alignment == 0u) normalized.alignment = PMM_ALLOCATION_GRANULE;
	if (normalized.size == 0u || (normalized.size & (PMM_ALLOCATION_GRANULE - 1u)) != 0u ||
	    normalized.alignment < PMM_ALLOCATION_GRANULE || (normalized.alignment & (normalized.alignment - 1u)) != 0u ||
	    (normalized.maximum_address != 0u && normalized.maximum_address <= normalized.minimum_address))
		return false;

	state = spinlock_lock_irqsave(&pmm_lock);
	if (!pmm_initialized || normalized.size > pmm_free_bytes) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}
	for (size_t i = 0u; i < pmm_range_count; i++) {
		size_t first;
		size_t count = normalized.size / PMM_ALLOCATION_GRANULE;

		if (pmm_ranges[i].free_granules < count || !pmm_find_free_extent(&pmm_ranges[i], &normalized, &first)) continue;
		for (size_t j = 0u; j < count; j++) pmm_bitmap_set(pmm_ranges[i].allocated, first + j);
		pmm_ranges[i].free_granules -= count;
		if (first == pmm_ranges[i].first_free_granule) {
			size_t next = first + count;
			while (next < pmm_ranges[i].granule_count && pmm_bitmap_test(pmm_ranges[i].allocated, next)) next++;
			pmm_ranges[i].first_free_granule = next;
		}
		pmm_free_bytes -= normalized.size;
		*out_extent = (struct pmm_extent){
			.address = pmm_ranges[i].address + first * PMM_ALLOCATION_GRANULE,
			.size    = normalized.size,
		};
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return true;
	}
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return false;
}

enum pmm_claim_result pmm_claim(struct pmm_extent extent) {
	struct pmm_range* range;
	struct irq_state  state;
	bool              intersects;
	size_t            first;
	size_t            count;

	if (!pmm_extent_valid(extent)) return PMM_CLAIM_UNAVAILABLE;
	state = spinlock_lock_irqsave(&pmm_lock);
	if (!pmm_initialized) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return PMM_CLAIM_UNAVAILABLE;
	}
	range = pmm_find_containing_range(extent.address, extent.size, &intersects);
	if (range == NULL) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return intersects ? PMM_CLAIM_UNAVAILABLE : PMM_CLAIM_NOT_MANAGED;
	}
	first = (extent.address - range->address) / PMM_ALLOCATION_GRANULE;
	count = extent.size / PMM_ALLOCATION_GRANULE;
	for (size_t i = 0u; i < count; i++) {
		if (!pmm_bitmap_test(range->allocated, first + i)) continue;
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return PMM_CLAIM_UNAVAILABLE;
	}
	for (size_t i = 0u; i < count; i++) pmm_bitmap_set(range->allocated, first + i);
	range->free_granules -= count;
	if (first == range->first_free_granule) {
		size_t next = first + count;
		while (next < range->granule_count && pmm_bitmap_test(range->allocated, next)) next++;
		range->first_free_granule = next;
	}
	pmm_free_bytes -= extent.size;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return PMM_CLAIM_OK;
}

bool pmm_free(struct pmm_extent extent) {
	struct pmm_range* range;
	struct irq_state  state;
	size_t            first;
	size_t            count;

	if (!pmm_extent_valid(extent)) return false;
	state = spinlock_lock_irqsave(&pmm_lock);
	if (!pmm_initialized || (range = pmm_find_containing_range(extent.address, extent.size, NULL)) == NULL) {
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}
	first = (extent.address - range->address) / PMM_ALLOCATION_GRANULE;
	count = extent.size / PMM_ALLOCATION_GRANULE;
	for (size_t i = 0u; i < count; i++) {
		if (pmm_bitmap_test(range->allocated, first + i) && !pmm_bitmap_test(range->reserved, first + i)) continue;
		spinlock_unlock_irqrestore(&pmm_lock, state);
		return false;
	}
	for (size_t i = 0u; i < count; i++) pmm_bitmap_clear(range->allocated, first + i);
	range->free_granules += count;
	if (first < range->first_free_granule) range->first_free_granule = first;
	pmm_free_bytes += extent.size;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return true;
}

size_t pmm_managed_range_count(void) {
	struct irq_state state  = spinlock_lock_irqsave(&pmm_lock);
	size_t           result = pmm_initialized ? pmm_range_count : 0u;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return result;
}

size_t pmm_total_size(void) {
	struct irq_state state  = spinlock_lock_irqsave(&pmm_lock);
	size_t           result = pmm_initialized ? pmm_total_bytes : 0u;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return result;
}

size_t pmm_free_size(void) {
	struct irq_state state  = spinlock_lock_irqsave(&pmm_lock);
	size_t           result = pmm_initialized ? pmm_free_bytes : 0u;
	spinlock_unlock_irqrestore(&pmm_lock, state);
	return result;
}
