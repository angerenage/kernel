#include <core/early_alloc.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 0x1000ull
#define EARLY_DEFAULT_ALIGN 16ull

struct early_arena {
	uint64_t base;
	uint64_t end;
	uint64_t cursor;
};

static struct early_arena* scratch_arena = NULL;
static struct early_arena* early_arena   = NULL;

static inline void* hhdm_phys_to_virt(uint64_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

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

static inline uint64_t normalize_align(size_t align) {
	uint64_t a = (uint64_t)align;
	if (a == 0) a = EARLY_DEFAULT_ALIGN;
	return a;
}

static inline bool align_up_u64(uint64_t v, uint64_t a, uint64_t* out) {
	uint64_t tmp;
	if (!a || (a & (a - 1))) return false;
	if (add_overflow_u64(v, a - 1, &tmp)) return false;
	*out = tmp & ~(a - 1);
	return true;
}

bool early_init(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset) {
	if (!memmap_resp || !memmap_resp->entry_count || !memmap_resp->entries) return false;

	boot_info.direct_map_offset = direct_map_offset;
	boot_info.range_count       = (size_t)memmap_resp->entry_count;

	uint64_t best_base = 0, best_end = 0, best_len = 0;
	uint64_t second_base = 0, second_end = 0, second_len = 0;
	for (uint64_t i = 0; i < memmap_resp->entry_count; i++) {
		struct limine_memmap_entry* e = memmap_resp->entries[i];

		if (e->type != LIMINE_MEMMAP_USABLE) continue;
		if (e->length < PAGE_SIZE) continue;

		uint64_t end;
		if (add_overflow_u64(e->base, e->length, &end)) continue;
		if (end <= e->base) continue;

		uint64_t len = end - e->base;
		if (len > best_len) {
			second_base = best_base;
			second_end  = best_end;
			second_len  = best_len;

			best_base = e->base;
			best_end  = end;
			best_len  = len;
		}
		else if (len > second_len) {
			second_base = e->base;
			second_end  = end;
			second_len  = len;
		}
	}

	if (best_len == 0) return false;

	uint64_t cursor;
	if (!align_up_u64(best_base, (uint64_t)_Alignof(struct mem_range), &cursor)) return false;

	uint64_t ranges_bytes64;
	if (mul_overflow_u64(memmap_resp->entry_count, (uint64_t)sizeof(struct mem_range), &ranges_bytes64)) return false;

	if (cursor > best_end) return false;
	if (ranges_bytes64 > best_end - cursor) {
		printf("Not enough memory to store memmap.\n");
		return false;
	}

	boot_info.ranges = (struct mem_range*)hhdm_phys_to_virt(cursor);
	for (uint64_t i = 0; i < memmap_resp->entry_count; i++) {
		struct limine_memmap_entry* e = memmap_resp->entries[i];
		boot_info.ranges[i]           = (struct mem_range){e->base, e->length, mem_range_type_from_limine(e->type)};
		if (add_overflow_u64(cursor, (uint64_t)sizeof(struct mem_range), &cursor)) return false;
	}

	if (second_len) {
		uint64_t arenas_ptr;
		if (!align_up_u64(second_base, (uint64_t)_Alignof(struct early_arena), &arenas_ptr)) return false;

		struct early_arena* arena = (struct early_arena*)hhdm_phys_to_virt(arenas_ptr);

		if (add_overflow_u64(arenas_ptr, 2ull * (uint64_t)sizeof(struct early_arena), &arenas_ptr)) return false;
		if (arenas_ptr > second_end) {
			printf("Not enough memory to store early arenas.\n");
			return false;
		}

		arena[0]    = (struct early_arena){best_base, best_end, cursor};
		early_arena = &arena[0];

		arena[1]      = (struct early_arena){second_base, second_end, arenas_ptr};
		scratch_arena = &arena[1];
	}
	else {
		if (!align_up_u64(cursor, (uint64_t)_Alignof(struct early_arena), &cursor)) return false;
		early_arena = (struct early_arena*)hhdm_phys_to_virt(cursor);

		if (add_overflow_u64(cursor, (uint64_t)sizeof(struct early_arena), &cursor)) return false;
		if (cursor > best_end) {
			printf("Not enough memory to store early arena.\n");
			return false;
		}

		*early_arena = (struct early_arena){best_base, best_end, cursor};
	}

	if (early_arena != NULL) {
		printf("early_arena = {.base = 0x%lx, .end = 0x%lx}\n", early_arena->base, early_arena->end);
	}
	if (scratch_arena != NULL) {
		printf("scratch_arena = {.base = 0x%lx, .end = 0x%lx}\n", scratch_arena->base, scratch_arena->end);
	}

	return true;
}

void* early_alloc(size_t size, size_t align) {
	uint64_t a = normalize_align(align);

	if (a == 0 || (a & (a - 1)) != 0) return NULL;

	uint64_t aligned;
	if (!align_up_u64(early_arena->cursor, a, &aligned)) return NULL;

	uint64_t new_cursor;
	if (add_overflow_u64(aligned, (uint64_t)size, &new_cursor)) return NULL;

	if (new_cursor > early_arena->end) return NULL;

	early_arena->cursor = new_cursor;
	return hhdm_phys_to_virt(aligned);
}

void* early_calloc(size_t nmemb, size_t size, size_t align) {
	uint64_t total;
	if (mul_overflow_u64((uint64_t)nmemb, (uint64_t)size, &total)) return NULL;

	void* p = early_alloc((size_t)total, align);
	if (!p) return NULL;

	memset(p, 0, (size_t)total);
	return p;
}

uint64_t early_remaining_bytes(void) {
	return early_arena->end - early_arena->cursor;
}

size_t early_reserved_range_count(void) {
	size_t count = 0;

	if (early_arena != NULL && early_arena->cursor > early_arena->base) count++;
	if (scratch_arena != NULL && scratch_arena->cursor > scratch_arena->base) count++;

	return count;
}

bool early_reserved_range(size_t index, struct early_reserved_range* out) {
	if (!out) return false;

	if (early_arena != NULL && early_arena->cursor > early_arena->base) {
		if (index == 0) {
			*out = (struct early_reserved_range){
				.base = (uintptr_t)early_arena->base,
				.end  = (uintptr_t)early_arena->cursor,
			};
			return true;
		}

		index--;
	}

	if (scratch_arena != NULL && scratch_arena->cursor > scratch_arena->base && index == 0) {
		*out = (struct early_reserved_range){
			.base = (uintptr_t)scratch_arena->base,
			.end  = (uintptr_t)scratch_arena->cursor,
		};
		return true;
	}

	return false;
}
