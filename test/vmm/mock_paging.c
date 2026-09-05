#include <base/vmm.h>
#include <core/pmm.h>
#include <hal/paging.h>
#include <string.h>

#include "test_support.h"

#define MOCK_PAGING_MAX_MAPPINGS 1024u
#define MOCK_PAGING_MAX_SPACES 64u

struct hal_paging_space {
	uintptr_t id;
	bool      allocated;
};

struct mock_mapping {
	const struct hal_paging_space* space;
	uintptr_t                      virt;
	uintptr_t                      phys;
	uint64_t                       flags;
	enum memory_type               memory_type;
	bool                           present;
};

static struct mock_mapping          mappings[MOCK_PAGING_MAX_MAPPINGS];
static struct hal_paging_space      spaces[MOCK_PAGING_MAX_SPACES];
static struct hal_paging_space      kernel_space    = {1u, true};
static const struct hal_paging_info paging_info     = {VMM_PAGE_SIZE, 1ull << 12};
static size_t                       fail_after_maps = (size_t)-1;
static size_t                       fail_map_budget = (size_t)-1;
static size_t                       successful_maps;
static bool                         initialized;
static bool                         fail_init_once;
static bool                         fail_next_unmap;
static uintptr_t                    next_space_id = 2u;

void mock_paging_reset(void) {
	memset(mappings, 0, sizeof(mappings));
	memset(spaces, 0, sizeof(spaces));
	fail_after_maps = (size_t)-1;
	fail_map_budget = (size_t)-1;
	successful_maps = 0u;
	initialized     = false;
	fail_init_once  = false;
	fail_next_unmap = false;
	kernel_space    = (struct hal_paging_space){1u, true};
	next_space_id   = 2u;
}

void mock_paging_fail_init_once(void) {
	fail_init_once = true;
}
void mock_paging_fail_after(size_t maps) {
	fail_after_maps = maps;
	fail_map_budget = (size_t)-1;
}
void mock_paging_fail_once_after(size_t maps) {
	fail_after_maps = maps;
	fail_map_budget = 1u;
}
void mock_paging_fail_next_unmap(void) {
	fail_next_unmap = true;
}

size_t mock_paging_mapping_count(void) {
	size_t count = 0u;
	for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++)
		if (mappings[i].present) count++;
	return count;
}

static struct mock_mapping* find_mapping(const struct hal_paging_space* space, uintptr_t virt) {
	uintptr_t page = virt & ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
	for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++)
		if (mappings[i].present && mappings[i].space == space && mappings[i].virt == page) return &mappings[i];
	return NULL;
}

bool hal_paging_init(void) {
	if (fail_init_once) {
		fail_init_once = false;
		return false;
	}
	mock_paging_reset();
	initialized = true;
	return true;
}

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && memory_type < MEMORY_TYPE_COUNT;
}

struct hal_paging_space* hal_paging_kernel_space(void) {
	return initialized ? &kernel_space : NULL;
}

bool hal_paging_space_create(struct hal_paging_space** out_space) {
	if (out_space == NULL || !initialized) return false;
	*out_space = NULL;
	for (size_t i = 0u; i < MOCK_PAGING_MAX_SPACES; i++) {
		if (spaces[i].allocated) continue;
		spaces[i]  = (struct hal_paging_space){next_space_id++, true};
		*out_space = &spaces[i];
		return true;
	}
	return false;
}

void hal_paging_space_destroy(struct hal_paging_space* space) {
	if (space == NULL || space == &kernel_space) return;
	for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++)
		if (mappings[i].space == space) mappings[i].present = false;
	*space = (struct hal_paging_space){0};
}

bool hal_paging_activate(const struct hal_paging_space* space) {
	return space != NULL && space->allocated;
}

static bool mock_map_should_fail(size_t pages) {
	if (successful_maps < fail_after_maps && pages <= fail_after_maps - successful_maps) return false;
	if (fail_map_budget != (size_t)-1) {
		if (fail_map_budget != 0u) fail_map_budget--;
		if (fail_map_budget == 0u) fail_after_maps = (size_t)-1;
	}
	return true;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	if (space == NULL || !space->allocated || !initialized || request == NULL ||
	    !hal_paging_mapping_supported(request->flags, request->memory_type) || request->size == 0u ||
	    ((request->virtual_address | request->physical_address | request->size) & (VMM_PAGE_SIZE - 1u)) != 0u ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address)
		return false;
	size_t pages = request->size / VMM_PAGE_SIZE;
	if (mock_map_should_fail(pages)) return false;
	if (MOCK_PAGING_MAX_MAPPINGS - mock_paging_mapping_count() < pages) return false;
	for (size_t offset = 0u; offset < request->size; offset += VMM_PAGE_SIZE)
		if (find_mapping(space, request->virtual_address + offset) != NULL) return false;
	for (size_t offset = 0u; offset < request->size; offset += VMM_PAGE_SIZE) {
		for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++) {
			if (mappings[i].present) continue;
			mappings[i] = (struct mock_mapping){space,
			                                    request->virtual_address + offset,
			                                    request->physical_address + offset,
			                                    request->flags,
			                                    request->memory_type,
			                                    true};
			break;
		}
	}
	successful_maps += pages;
	return true;
}

bool hal_paging_remap(struct hal_paging_space* space, const struct hal_paging_remap_request* request) {
	if (space == NULL || !space->allocated || !initialized || request == NULL || request->size == 0u ||
	    ((request->virtual_address | request->physical_address | request->size) & (VMM_PAGE_SIZE - 1u)) != 0u ||
	    request->size > UINTPTR_MAX - request->virtual_address ||
	    request->size > UINTPTR_MAX - request->physical_address)
		return false;
	for (size_t offset = 0u; offset < request->size; offset += VMM_PAGE_SIZE)
		if (find_mapping(space, request->virtual_address + offset) == NULL) return false;
	for (size_t offset = 0u; offset < request->size; offset += VMM_PAGE_SIZE)
		find_mapping(space, request->virtual_address + offset)->phys = request->physical_address + offset;
	return true;
}

static bool valid_range(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	return space != NULL && space->allocated && initialized && size != 0u &&
	       ((virt | size) & (VMM_PAGE_SIZE - 1u)) == 0u && size <= UINTPTR_MAX - virt;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	if (!valid_range(space, virt, size)) return false;
	if (fail_next_unmap) {
		fail_next_unmap = false;
		return false;
	}
	uintptr_t end = virt + size;
	for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++)
		if (mappings[i].present && mappings[i].space == space && mappings[i].virt >= virt && mappings[i].virt < end)
			mappings[i].present = false;
	return true;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	if (!valid_range(space, virt, size) || (flags & ~HAL_PAGE_VALID_MASK) != 0u) return false;
	uintptr_t end = virt + size;
	for (size_t i = 0u; i < MOCK_PAGING_MAX_MAPPINGS; i++)
		if (mappings[i].present && mappings[i].space == space && mappings[i].virt >= virt && mappings[i].virt < end)
			mappings[i].flags = flags;
	return true;
}

bool hal_paging_query(const struct hal_paging_space* space, uintptr_t virt,
                      struct hal_paging_translation* out_translation) {
	if (out_translation != NULL) *out_translation = (struct hal_paging_translation){0};
	if (space == NULL || !space->allocated || !initialized) return false;
	struct mock_mapping* mapping = find_mapping(space, virt);
	if (mapping == NULL) return false;
	if (out_translation != NULL)
		*out_translation = (struct hal_paging_translation){
			mapping->phys + (virt & (VMM_PAGE_SIZE - 1u)), VMM_PAGE_SIZE, mapping->flags, mapping->memory_type};
	return true;
}
