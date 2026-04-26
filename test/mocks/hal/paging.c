#include <hal/paging.h>

__attribute__((weak))
bool hal_paging_init(void) {
	return true;
}

__attribute__((weak))
struct hal_address_space* hal_paging_kernel_space(void) {
	static struct hal_address_space kernel_space = {.lower_root_phys = 1u};
	return &kernel_space;
}

__attribute__((weak))
bool hal_paging_space_create(struct hal_address_space* out_space) {
	if (out_space == NULL) return false;
	*out_space = (struct hal_address_space){.lower_root_phys = 2u};
	return true;
}

__attribute__((weak))
void hal_paging_space_destroy(struct hal_address_space* space) {
	if (space != NULL) *space = (struct hal_address_space){0};
}

__attribute__((weak))
bool hal_paging_activate(const struct hal_address_space* space) {
	return space != NULL && space->lower_root_phys != 0u;
}

__attribute__((weak))
bool hal_paging_map(struct hal_address_space* space, uintptr_t virt, uintptr_t phys,
                                          uint64_t flags) {
	(void)space;
	(void)virt;
	(void)phys;
	(void)flags;
	return false;
}

__attribute__((weak))
bool hal_paging_unmap(struct hal_address_space* space, uintptr_t virt) {
	(void)space;
	(void)virt;
	return false;
}

__attribute__((weak))
bool hal_paging_query(const struct hal_address_space* space, uintptr_t virt, uintptr_t* out_phys,
                                            uint64_t* out_flags) {
	(void)space;
	(void)virt;
	if (out_phys != NULL) *out_phys = 0u;
	if (out_flags != NULL) *out_flags = 0u;
	return false;
}
