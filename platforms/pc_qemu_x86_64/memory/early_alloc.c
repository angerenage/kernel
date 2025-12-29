#include "hal/early_alloc.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 0x1000ull

struct early_arena {
	uintptr_t base;
	uintptr_t end;
	uintptr_t cursor;
};

static struct early_arena* scratch_arena = NULL;
static struct early_arena* early_arena   = NULL;
static uint64_t            hhdm_offset;

static inline void* hhdm_phys_to_virt(uint64_t phys) {
	return (void*)(uintptr_t)(phys + hhdm_offset);
}

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline uint64_t align_down_u64(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

bool early_init(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset) {
	if (!memmap_resp || !memmap_resp->entry_count || !memmap_resp->entries) return false;

	hhdm_offset = direct_map_offset;

	uint64_t best_base = 0, best_end = 0, best_len = 0;
	uint64_t second_base = 0, second_end = 0, second_len = 0;
	for (uint64_t i = 0; i < memmap_resp->entry_count; i++) {
		struct limine_memmap_entry* e = memmap_resp->entries[i];

		if (e->type != LIMINE_MEMMAP_USABLE) continue;
		if (e->length < PAGE_SIZE) continue;
		if (e->base > UINT64_MAX - e->length) continue;

		uint64_t base = align_up_u64(e->base, PAGE_SIZE);
		uint64_t end  = align_down_u64(e->base + e->length, PAGE_SIZE);
		if (end <= base) continue;

		uint64_t len = end - base;
		if (len > best_len) {
			second_base = best_base;
			second_end  = best_end;
			second_len  = best_len;

			best_base = base;
			best_end  = end;
			best_len  = len;
		}
		else if (len > second_len) {
			second_base = base;
			second_end  = end;
			second_len  = len;
		}
	}

	if (best_len == 0) return false;

	if (second_len) {
		struct early_arena* arena = (struct early_arena*)hhdm_phys_to_virt(second_base);
		arena[0].base             = best_base;
		arena[0].end              = best_end;
		arena[0].cursor           = best_base;
		early_arena               = &arena[0];

		arena[1].base   = second_base;
		arena[1].end    = second_end;
		arena[1].cursor = second_base + PAGE_SIZE;
		scratch_arena   = &arena[1];
	}
	else {
		early_arena         = (struct early_arena*)hhdm_phys_to_virt(best_base);
		early_arena->base   = best_base;
		early_arena->end    = best_end;
		early_arena->cursor = best_base + PAGE_SIZE;
	}

	if (early_arena != NULL) {
		printf("early_arena = {.base = 0x%lx, .end = 0x%lx}\n", early_arena->base, early_arena->end);
	}
	if (scratch_arena != NULL) {
		printf("scratch_arena = {.base = 0x%lx, .end = 0x%lx}\n", scratch_arena->base, scratch_arena->end);
	}

	return true;
}
