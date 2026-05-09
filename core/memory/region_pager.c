#include <core/backing_store.h>
#include <core/memory_region.h>
#include <core/page_table.h>
#include <core/pmm.h>
#include <core/region_pager.h>
#include <core/vaddr_alloc.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void restore_live_mappings(struct address_space* space, const struct memory_region* region, vmm_prot_t prot,
                                  bool replace_existing) {
	struct hal_address_space* table;

	if (!space || !region) return;
	table = address_space_hal(space);
	if (table == NULL) return;
	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry         = backing_store_entry(&region->backing, page);
		uintptr_t virt          = region->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t existing_phys = 0;
		uintptr_t phys;

		if (!backing_page_is_mapped(entry)) continue;
		phys = backing_page_phys(entry);
		if (page_table_query(table, virt, &existing_phys, NULL)) {
			if (!replace_existing) continue;
			if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) continue;
			(void)page_table_unmap(table, virt);
		}
		(void)page_table_map(table, virt, phys, prot);
	}
}

static bool map_one(struct address_space* space, struct memory_region* region, size_t page_index) {
	struct hal_address_space* table;
	uintptr_t                 entry;
	uintptr_t                 phys;
	uintptr_t                 virt;
	uintptr_t                 existing_phys  = 0;
	bool                      allocated_phys = false;

	if (!space || !region || page_index >= region->page_count) return false;
	table = address_space_hal(space);
	if (table == NULL) return false;
	if (!backing_store_ensure_page(&region->backing, page_index, &phys, &allocated_phys)) return false;

	entry = backing_store_entry(&region->backing, page_index);
	if (backing_page_is_mapped(entry)) return true;

	virt = region->base + page_index * (uintptr_t)PMM_PAGE_SIZE;
	if (page_table_query(table, virt, &existing_phys, NULL)) {
		if ((existing_phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != phys) {
			if (allocated_phys) (void)pmm_free_pages(phys, 1);
			backing_store_release_if_empty(&region->backing);
			return false;
		}
		backing_store_set_entry(
			&region->backing, page_index, backing_page_make(phys, backing_page_flags(entry) | BACKING_PAGE_MAPPED));
		backing_store_increment_mapped(&region->backing);
		return true;
	}

	if (!page_table_map(table, virt, phys, region->prot)) {
		if (allocated_phys) (void)pmm_free_pages(phys, 1);
		backing_store_release_if_empty(&region->backing);
		return false;
	}

	backing_store_set_entry(
		&region->backing, page_index, backing_page_make(phys, backing_page_flags(entry) | BACKING_PAGE_MAPPED));
	backing_store_increment_mapped(&region->backing);
	return true;
}

bool region_pager_map_all(struct address_space* space, struct memory_region* region) {
	if (!space || !region) return false;
	if (region->page_count == 0) return false;
	if (memory_region_state(region) == VMM_STATE_MAPPED) return true;
	if (address_space_hal(space) == NULL) return false;

	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry;

		if (!backing_store_ensure(&region->backing)) goto rollback;
		entry = backing_store_entry(&region->backing, page);
		if (backing_page_is_mapped(entry)) {
			backing_store_set_entry(&region->backing, page, entry | BACKING_PAGE_ROLLBACK_SKIP);
			continue;
		}
		if (backing_page_has_phys(entry)) {
			backing_store_set_entry(
				&region->backing,
				page,
				backing_page_make(backing_page_phys(entry), backing_page_flags(entry) | BACKING_PAGE_ROLLBACK_KEEP));
		}
		if (!map_one(space, region, page)) goto rollback;
	}

	for (size_t page = 0; page < region->page_count; page++) {
		backing_store_set_entry(&region->backing,
		                        page,
		                        backing_store_entry(&region->backing, page) &
		                            ~(BACKING_PAGE_ROLLBACK_KEEP | BACKING_PAGE_ROLLBACK_SKIP));
	}
	return true;

rollback:
	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry = backing_store_entry(&region->backing, page);
		uintptr_t virt  = region->base + page * (uintptr_t)PMM_PAGE_SIZE;

		if ((backing_page_flags(entry) & BACKING_PAGE_ROLLBACK_SKIP) != 0) {
			backing_store_set_entry(&region->backing, page, entry & ~BACKING_PAGE_ROLLBACK_SKIP);
			continue;
		}
		if (!backing_page_is_mapped(entry)) continue;
		(void)page_table_unmap(address_space_hal(space), virt);
		if ((backing_page_flags(entry) & BACKING_PAGE_ROLLBACK_KEEP) != 0) {
			backing_store_set_entry(&region->backing, page, backing_page_make(backing_page_phys(entry), 0));
		}
		else {
			(void)pmm_free_pages(backing_page_phys(entry), 1);
			backing_store_set_entry(&region->backing, page, 0);
		}
		backing_store_decrement_mapped(&region->backing);
	}
	backing_store_release_if_empty(&region->backing);
	return false;
}

bool region_pager_handle_lazy_fault(struct address_space* space, struct memory_region* region, uintptr_t fault_addr) {
	size_t    page_index;
	uintptr_t entry;

	if (!space || !region || (region->map_flags & (uint64_t)VMM_MAP_LAZY) == 0) return false;
	if ((uint64_t)fault_addr < (uint64_t)region->base) return false;
	page_index = ((uintptr_t)fault_addr - region->base) / (uintptr_t)PMM_PAGE_SIZE;
	if (page_index >= region->page_count) return false;
	if (!memory_region_stack_fault_is_valid(region, page_index)) return false;
	entry = backing_store_entry(&region->backing, page_index);
	if (backing_page_is_mapped(entry)) return false;
	return map_one(space, region, page_index);
}

bool region_pager_unmap_all(struct address_space* space, struct memory_region* region, bool release_phys) {
	struct hal_address_space* table;

	if (!space || !region || backing_store_mapped_count(&region->backing) == 0) return false;
	table = address_space_hal(space);
	if (table == NULL) return false;
	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry = backing_store_entry(&region->backing, page);
		uintptr_t virt  = region->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys  = 0;

		if (!backing_page_is_mapped(entry)) continue;
		if (!page_table_query(table, virt, &phys, NULL)) {
			restore_live_mappings(space, region, region->prot, false);
			return false;
		}
		if ((phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u)) != backing_page_phys(entry)) {
			restore_live_mappings(space, region, region->prot, false);
			return false;
		}
		if (!page_table_unmap(table, virt)) {
			restore_live_mappings(space, region, region->prot, false);
			return false;
		}
	}
	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry = backing_store_entry(&region->backing, page);
		if (!backing_page_is_mapped(entry)) continue;
		backing_store_set_entry(
			&region->backing,
			page,
			backing_page_make(backing_page_phys(entry), backing_page_flags(entry) & ~BACKING_PAGE_MAPPED));
	}
	backing_store_set_mapped_count(&region->backing, 0);
	if (release_phys) backing_store_release(&region->backing);
	return true;
}

bool region_pager_protect_all(struct address_space* space, struct memory_region* region, vmm_prot_t new_prot) {
	vmm_prot_t                old_prot;
	struct hal_address_space* table;

	if (!space || !region) return false;
	table = address_space_hal(space);
	if (table == NULL) return false;
	if (region->prot == new_prot) return true;
	if (memory_region_state(region) == VMM_STATE_RESERVED) {
		region->prot = new_prot;
		return true;
	}
	old_prot = region->prot;
	for (size_t page = 0; page < region->page_count; page++) {
		uintptr_t entry = backing_store_entry(&region->backing, page);
		uintptr_t virt  = region->base + page * (uintptr_t)PMM_PAGE_SIZE;
		uintptr_t phys;

		if (!backing_page_is_mapped(entry)) continue;
		phys = backing_page_phys(entry);
		if (!page_table_remap_prot(table, virt, phys, new_prot)) {
			restore_live_mappings(space, region, old_prot, true);
			return false;
		}
	}
	region->prot = new_prot;
	return true;
}

bool region_pager_unmap_space(struct address_space* space) {
	bool ok = true;

	if (!space) return false;
	for (size_t i = 0; i < memory_region_capacity(space); i++) {
		struct memory_region* region = memory_region_at(space, i);

		if (!memory_region_is_used(region)) continue;
		if (backing_store_mapped_count(&region->backing) == 0) continue;
		if (!region_pager_unmap_all(space, region, false)) ok = false;
	}
	return ok;
}
