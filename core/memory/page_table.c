#include <core/page_table.h>
#include <core/pmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stdint.h>

static inline uint64_t prot_to_hal_flags(vmm_prot_t prot) {
	uint64_t flags = 0;

	if ((prot & VMM_PROT_WRITE) != 0) flags |= HAL_PAGE_WRITE;
	if ((prot & VMM_PROT_EXEC) != 0) flags |= HAL_PAGE_EXEC;
	if ((prot & VMM_PROT_GLOBAL) != 0) flags |= HAL_PAGE_GLOBAL;
	if ((prot & VMM_PROT_NO_CACHE) != 0) flags |= HAL_PAGE_NO_CACHE;
	if ((prot & VMM_PROT_USER) != 0) flags |= HAL_PAGE_USER;
	return flags;
}

bool page_table_map(struct hal_address_space* table, uintptr_t virt, uintptr_t phys, vmm_prot_t prot) {
	if (table == NULL) return false;
	return hal_paging_map(table, virt, phys, prot_to_hal_flags(prot));
}

bool page_table_unmap(struct hal_address_space* table, uintptr_t virt) {
	if (table == NULL) return false;
	return hal_paging_unmap(table, virt);
}

bool page_table_query(const struct hal_address_space* table, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags) {
	if (table == NULL) return false;
	return hal_paging_query(table, virt, out_phys, out_flags);
}

bool page_table_remap_prot(struct hal_address_space* table, uintptr_t virt, uintptr_t phys, vmm_prot_t prot) {
	if (table == NULL) return false;
	if (!hal_paging_unmap(table, virt)) return false;
	return hal_paging_map(table, virt, phys, prot_to_hal_flags(prot));
}
