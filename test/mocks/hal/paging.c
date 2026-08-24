#include <hal/paging.h>

static uintptr_t hal_paging_mock_active_root;

uintptr_t hal_paging_mock_active_root_phys(void) {
	return hal_paging_mock_active_root;
}

void hal_paging_mock_reset_active(void) {
	hal_paging_mock_active_root = 0u;
}

bool hal_paging_init(void) {
	hal_paging_mock_active_root = 0u;
	return true;
}

struct hal_address_space* hal_paging_kernel_space(void) {
	static struct hal_address_space kernel_space = {.lower_root_phys = 1u};
	return &kernel_space;
}

bool hal_paging_space_create(struct hal_address_space* out_space) {
	if (out_space == NULL) return false;
	*out_space = (struct hal_address_space){.lower_root_phys = 2u};
	return true;
}

void hal_paging_space_destroy(struct hal_address_space* space) {
	if (space != NULL) *space = (struct hal_address_space){0};
}

bool hal_paging_activate(const struct hal_address_space* space) {
	if (space == NULL || space->lower_root_phys == 0u) return false;
	hal_paging_mock_active_root = space->lower_root_phys;
	return true;
}

bool hal_paging_map(struct hal_address_space* space, uintptr_t virt, uintptr_t phys, uint64_t flags,
                    enum memory_type memory_type) {
	(void)space;
	(void)virt;
	(void)phys;
	(void)flags;
	(void)memory_type;
	return false;
}

bool hal_paging_unmap_range(struct hal_address_space* space, uintptr_t virt, size_t page_count) {
	(void)space;
	(void)virt;
	return page_count != 0u;
}

bool hal_paging_protect_range(struct hal_address_space* space, uintptr_t virt, size_t page_count, uint64_t flags) {
	(void)space;
	(void)virt;
	(void)flags;
	return page_count != 0u;
}

bool hal_paging_query(const struct hal_address_space* space, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	(void)space;
	(void)virt;
	if (out_phys != NULL) *out_phys = 0u;
	if (out_flags != NULL) *out_flags = 0u;
	return false;
}
