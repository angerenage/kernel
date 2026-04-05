#include <core/pmm.h>
#include <hal/paging.h>

#include "test_support.h"

#define MOCK_PAGING_MAX_MAPPINGS 1024u

struct mock_mapping {
	uintptr_t virt;
	uintptr_t phys;
	uint64_t  flags;
	bool      present;
};

static struct mock_mapping mappings[MOCK_PAGING_MAX_MAPPINGS];
static size_t              fail_after_maps = (size_t)-1;
static size_t              fail_map_budget = (size_t)-1;
static size_t              successful_maps;
static bool                initialized;

void mock_paging_reset(void) {
	for (size_t i = 0; i < MOCK_PAGING_MAX_MAPPINGS; i++) {
		mappings[i].virt    = 0;
		mappings[i].phys    = 0;
		mappings[i].flags   = 0;
		mappings[i].present = false;
	}

	fail_after_maps = (size_t)-1;
	fail_map_budget = (size_t)-1;
	successful_maps = 0;
	initialized     = false;
}

void mock_paging_fail_after(size_t maps) {
	fail_after_maps = maps;
	fail_map_budget = (size_t)-1;
}

void mock_paging_fail_once_after(size_t maps) {
	fail_after_maps = maps;
	fail_map_budget = 1;
}

size_t mock_paging_mapping_count(void) {
	size_t count = 0;

	for (size_t i = 0; i < MOCK_PAGING_MAX_MAPPINGS; i++) {
		if (mappings[i].present) count++;
	}

	return count;
}

static struct mock_mapping* find_mapping(uintptr_t virt) {
	uintptr_t page_base = virt & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);

	for (size_t i = 0; i < MOCK_PAGING_MAX_MAPPINGS; i++) {
		if (!mappings[i].present) continue;
		if (mappings[i].virt == page_base) return &mappings[i];
	}

	return NULL;
}

bool hal_paging_init(void) {
	mock_paging_reset();
	initialized = true;
	return true;
}

bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
	uintptr_t page_base = virt & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if ((phys & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (find_mapping(virt) != NULL) return false;
	if (successful_maps >= fail_after_maps) {
		if (fail_map_budget != (size_t)-1) {
			if (fail_map_budget == 0) {
				fail_after_maps = (size_t)-1;
			}
			else {
				fail_map_budget--;
				if (fail_map_budget == 0) fail_after_maps = (size_t)-1;
			}
		}
		return false;
	}

	for (size_t i = 0; i < MOCK_PAGING_MAX_MAPPINGS; i++) {
		if (mappings[i].present) continue;

		mappings[i] = (struct mock_mapping){
			.virt    = page_base,
			.phys    = phys,
			.flags   = flags,
			.present = true,
		};
		successful_maps++;
		return true;
	}

	return false;
}

bool hal_paging_unmap(uintptr_t virt) {
	struct mock_mapping* mapping;

	if (!initialized) return false;
	if ((virt & (PMM_PAGE_SIZE - 1u)) != 0) return false;

	mapping = find_mapping(virt);
	if (!mapping) return false;

	mapping->present = false;
	return true;
}

bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	struct mock_mapping* mapping;
	uintptr_t            page_offset = virt & (uintptr_t)(PMM_PAGE_SIZE - 1u);

	if (out_phys) *out_phys = 0;
	if (out_flags) *out_flags = 0;
	if (!initialized) return false;

	mapping = find_mapping(virt);
	if (!mapping) return false;

	if (out_phys) *out_phys = mapping->phys + page_offset;
	if (out_flags) *out_flags = mapping->flags;
	return true;
}
