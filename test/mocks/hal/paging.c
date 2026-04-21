#include <hal/paging.h>

__attribute__((weak))
bool hal_paging_init(void) {
	return true;
}

__attribute__((weak))
bool hal_paging_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
	(void)virt;
	(void)phys;
	(void)flags;
	return false;
}

__attribute__((weak))
bool hal_paging_unmap(uintptr_t virt) {
	(void)virt;
	return false;
}

__attribute__((weak))
bool hal_paging_query(uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	(void)virt;
	if (out_phys != NULL) *out_phys = 0u;
	if (out_flags != NULL) *out_flags = 0u;
	return false;
}
