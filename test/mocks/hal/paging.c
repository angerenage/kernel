#include <hal/paging.h>

struct hal_paging_space {
	uintptr_t root_phys;
};

static uintptr_t                    hal_paging_mock_active_root;
static struct hal_paging_space      kernel_space = {1u};
static struct hal_paging_space      user_space   = {2u};
static const struct hal_paging_info paging_info  = {4096u, 1ull << 12};

struct hal_paging_space* hal_paging_mock_space(uintptr_t root_phys) {
	user_space.root_phys = root_phys;
	return &user_space;
}

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

const struct hal_paging_info* hal_paging_info(void) {
	return &paging_info;
}

bool hal_paging_mapping_supported(uint64_t flags, enum memory_type memory_type) {
	return (flags & ~HAL_PAGE_VALID_MASK) == 0u && memory_type < MEMORY_TYPE_COUNT;
}

struct hal_paging_space* hal_paging_kernel_space(void) {
	return &kernel_space;
}

bool hal_paging_space_create(struct hal_paging_space** out_space) {
	if (out_space == NULL) return false;
	*out_space = &user_space;
	return true;
}

void hal_paging_space_destroy(struct hal_paging_space* space) {
	(void)space;
}

bool hal_paging_activate(const struct hal_paging_space* space) {
	if (space == NULL || space->root_phys == 0u) return false;
	hal_paging_mock_active_root = space->root_phys;
	return true;
}

bool hal_paging_map(struct hal_paging_space* space, const struct hal_paging_map_request* request) {
	(void)space;
	(void)request;
	return false;
}

bool hal_paging_remap(struct hal_paging_space* space, const struct hal_paging_remap_request* request) {
	(void)space;
	(void)request;
	return false;
}

bool hal_paging_unmap(struct hal_paging_space* space, uintptr_t virt, size_t size) {
	(void)space;
	(void)virt;
	return size != 0u;
}

bool hal_paging_protect(struct hal_paging_space* space, uintptr_t virt, size_t size, uint64_t flags) {
	(void)space;
	(void)virt;
	(void)flags;
	return size != 0u;
}

bool hal_paging_query(const struct hal_paging_space* space, uintptr_t virt,
                      struct hal_paging_translation* out_translation) {
	(void)space;
	(void)virt;
	if (out_translation != NULL) *out_translation = (struct hal_paging_translation){0};
	return false;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}
