#include <base/process.h>
#include <core/lock.h>
#include <core/memory_region.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/region_pager.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VMM_WINDOW_BASE MM_KERNEL_VMM_BASE
#define VMM_WINDOW_SIZE MM_KERNEL_VMM_SIZE

static bool            initialized;
static struct spinlock vmm_lock =
	SPINLOCK_INIT_CLASS("vmm_lock", SPINLOCK_ORDER_VMM, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static inline bool prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0;
}

bool vmm_init(void) {
	size_t           window_pages = VMM_WINDOW_SIZE / PMM_PAGE_SIZE;
	struct irq_state state;

	state       = spinlock_lock_irqsave(&vmm_lock);
	initialized = false;
	(void)region_pager_unmap_space(address_space_kernel());
	memory_region_destroy_all(address_space_kernel());
	if (!hal_paging_init()) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	address_space_deinit(address_space_kernel());
	if (!address_space_init(address_space_kernel(), (uintptr_t)VMM_WINDOW_BASE, window_pages)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (hal_paging_kernel_space() == NULL) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	address_space_kernel()->hal_space = *hal_paging_kernel_space();
	if (!memory_region_ensure_capacity(address_space_kernel())) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	initialized = true;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

static bool vmm_alloc_internal(struct address_space* space, uintptr_t requested_base,
                               const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base) {
	struct memory_region_create_result result;
	struct irq_state                   state;

	if (out_id) *out_id = VMM_ID_INVALID;
	if (out_base) *out_base = NULL;
	if (!initialized || !space || !params || (!out_id && !out_base) || params->page_count == 0) return false;
	if (!prot_is_valid(params->prot)) return false;
	if ((params->map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0) return false;

	state = spinlock_lock_irqsave(&vmm_lock);
	if (!memory_region_create(space, requested_base, params, &result)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if ((params->map_flags & VMM_MAP_LAZY) == 0 && !region_pager_map_all(space, result.region)) {
		(void)memory_region_destroy(space, result.region);
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (out_id) *out_id = result.region->id;
	if (out_base) *out_base = (void*)result.base;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_alloc(struct address_space* space, const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base) {
	return vmm_alloc_internal(space, 0u, params, out_id, out_base);
}

bool vmm_alloc_at(struct address_space* space, void* base, const struct vmm_alloc_params* params, vmm_id_t* out_id) {
	void*    allocated_base = NULL;
	vmm_id_t id             = VMM_ID_INVALID;

	if (base == NULL) {
		if (out_id) *out_id = VMM_ID_INVALID;
		return false;
	}
	if (!vmm_alloc_internal(space, (uintptr_t)base, params, &id, &allocated_base)) return false;
	if (out_id) *out_id = id;
	if (allocated_base != base) {
		(void)vmm_free(space, id);
		if (out_id) *out_id = VMM_ID_INVALID;
		return false;
	}
	return allocated_base == base;
}

bool vmm_alloc_phys(struct address_space* space, uintptr_t phys_base, size_t page_count, vmm_prot_t prot,
                    void* virt_base, vmm_id_t* out_id, void** out_base) {
	struct memory_region*   region;
	vmm_id_t                id          = VMM_ID_INVALID;
	void*                   region_base = NULL;
	struct vmm_alloc_params params;
	struct irq_state        state;

	if (out_id) *out_id = VMM_ID_INVALID;
	if (out_base) *out_base = NULL;
	if (!initialized || !space || page_count == 0) return false;
	if (!prot_is_valid(prot)) return false;
	if (out_id == NULL && out_base == NULL) return false;

	params = (struct vmm_alloc_params){
		.page_count = page_count,
		.prot       = prot,
		.kind       = VMM_KIND_PHYSICAL,
	};

	state = spinlock_lock_irqsave(&vmm_lock);
	if (!memory_region_create_phys(
			space, (uintptr_t)virt_base, phys_base, &params, &region, (uintptr_t*)&region_base)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (!region_pager_map_all(space, region)) {
		(void)memory_region_destroy(space, region);
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	id = region->id;
	if (out_id) *out_id = id;
	if (out_base) *out_base = region_base;
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_free(struct address_space* space, vmm_id_t id) {
	struct memory_region* region;
	struct irq_state      state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;
	state  = spinlock_lock_irqsave(&vmm_lock);
	region = memory_region_find_by_id(space, id);
	if (!region) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	if (region_presence_mapped_count(&region->presence) != 0 && !region_pager_unmap_all(space, region, false)) {
		spinlock_unlock_irqrestore(&vmm_lock, state);
		return false;
	}
	(void)memory_region_destroy(space, region);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return true;
}

bool vmm_map(struct address_space* space, vmm_id_t id) {
	struct memory_region* region;
	bool                  ok;
	struct irq_state      state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;
	state  = spinlock_lock_irqsave(&vmm_lock);
	region = memory_region_find_by_id(space, id);
	ok     = region_pager_map_all(space, region);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_unmap(struct address_space* space, vmm_id_t id, bool release_phys) {
	struct memory_region* region;
	bool                  ok;
	struct irq_state      state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;
	state  = spinlock_lock_irqsave(&vmm_lock);
	region = memory_region_find_by_id(space, id);
	ok     = region_pager_unmap_all(space, region, release_phys);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_protect(struct address_space* space, vmm_id_t id, vmm_prot_t new_prot) {
	struct memory_region* region;
	bool                  ok;
	struct irq_state      state;

	if (!initialized || !space || id == VMM_ID_INVALID) return false;
	if (!prot_is_valid(new_prot)) return false;
	if (!memory_region_params_allowed(space,
	                                  &(const struct vmm_alloc_params){
										  .page_count = 1,
										  .prot       = new_prot,
										  .kind       = VMM_KIND_GENERIC,
									  })) {
		return false;
	}
	state  = spinlock_lock_irqsave(&vmm_lock);
	region = memory_region_find_by_id(space, id);
	ok     = region_pager_protect_all(space, region, new_prot);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return ok;
}

bool vmm_resolve_page_fault(struct address_space* space, uintptr_t addr) {
	struct vmm_transfer_guard guard = {0};
	bool                      ok;

	if (!initialized || !space) return false;
	vmm_transfer_guard_acquire(&guard);
	ok = vmm_resolve_page_fault_guarded(&guard, space, addr);
	vmm_transfer_guard_release(&guard);
	return ok;
}

void vmm_transfer_guard_acquire(struct vmm_transfer_guard* guard) {
	if (guard == NULL) return;
	guard->active    = false;
	guard->irq_state = spinlock_lock_irqsave(&vmm_lock);
	guard->active    = true;
}

void vmm_transfer_guard_release(struct vmm_transfer_guard* guard) {
	if (guard == NULL || !guard->active) return;
	guard->active = false;
	spinlock_unlock_irqrestore(&vmm_lock, guard->irq_state);
}

bool vmm_resolve_page_fault_guarded(const struct vmm_transfer_guard* guard, struct address_space* space,
                                    uintptr_t addr) {
	struct memory_region* region;
	uintptr_t             page_base;

	if (guard == NULL || !guard->active || !initialized || !space) return false;
	page_base = addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	if (hal_paging_query(address_space_hal(space), page_base, NULL, NULL)) return false;
	region = memory_region_find_containing(space, addr);
	return region_pager_handle_lazy_fault(space, region, addr);
}

static bool fault_access_allowed(const struct memory_region* region, enum vmm_fault_access access, bool user_mode) {
	if (region == NULL) return false;
	if (user_mode && (region->prot & VMM_PROT_USER) == 0u) return false;
	switch (access) {
	case VMM_FAULT_ACCESS_READ:
		return (region->prot & VMM_PROT_READ) != 0u;
	case VMM_FAULT_ACCESS_WRITE:
		return (region->prot & VMM_PROT_WRITE) != 0u;
	case VMM_FAULT_ACCESS_EXEC:
		return (region->prot & VMM_PROT_EXEC) != 0u;
	case VMM_FAULT_ACCESS_UNKNOWN:
	default:
		return true;
	}
}

static bool resolve_lazy_fault_in_space(struct address_space* space, uintptr_t addr, enum vmm_fault_access access,
                                        bool user_mode) {
	struct vmm_transfer_guard guard = {0};
	struct memory_region*     region;
	bool                      resolved = false;

	if (space == NULL) return false;
	vmm_transfer_guard_acquire(&guard);
	region = memory_region_find_containing(space, addr);
	if (fault_access_allowed(region, access, user_mode)) {
		resolved = vmm_resolve_page_fault_guarded(&guard, space, addr);
	}
	vmm_transfer_guard_release(&guard);
	return resolved;
}

static bool resolve_current_lazy_fault(uintptr_t addr, enum vmm_fault_access access, bool user_mode) {
	struct thread*        current;
	struct address_space* current_space;
	struct address_space* kernel_space;

	current       = sched_current_thread();
	current_space = current == NULL ? NULL : current->address_space;
	kernel_space  = address_space_kernel();
	if (current_space != NULL && resolve_lazy_fault_in_space(current_space, addr, access, user_mode)) return true;
	if (user_mode) return false;
	if (current_space == kernel_space) return false;
	return resolve_lazy_fault_in_space(kernel_space, addr, access, false);
}

static enum vmm_fault_kind classify_current_page_fault(uintptr_t addr, bool user_mode) {
	struct thread*        current;
	struct address_space* current_space;
	struct address_space* kernel_space;
	struct irq_state      state;
	uintptr_t             page_base;
	bool                  mapped = false;

	current       = sched_current_thread();
	current_space = current == NULL ? NULL : current->address_space;
	kernel_space  = address_space_kernel();
	page_base     = addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	state         = spinlock_lock_irqsave(&vmm_lock);
	if (current_space != NULL) {
		mapped = hal_paging_query(address_space_hal(current_space), page_base, NULL, NULL);
	}
	if (!mapped && !user_mode && current_space != kernel_space) {
		mapped = hal_paging_query(address_space_hal(kernel_space), page_base, NULL, NULL);
	}
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return mapped ? VMM_FAULT_PROTECTION : VMM_FAULT_NOT_PRESENT;
}

static uintptr_t vmm_fault_exit_code(enum vmm_fault_kind kind) {
	switch (kind) {
	case VMM_FAULT_NOT_PRESENT:
		return PROCESS_EXIT_MEMORY_NOT_PRESENT;
	case VMM_FAULT_PROTECTION:
		return PROCESS_EXIT_MEMORY_PROTECTION;
	case VMM_FAULT_INVALID:
	default:
		return PROCESS_EXIT_MEMORY_INVALID;
	}
}

bool vmm_handle_current_page_fault(uintptr_t addr, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                   bool user_mode) {
	struct process* process;

	if (kind == VMM_FAULT_UNCLASSIFIED) kind = classify_current_page_fault(addr, user_mode);
	if (kind == VMM_FAULT_NOT_PRESENT && resolve_current_lazy_fault(addr, access, user_mode)) return true;
	if (!user_mode) return false;
	process = process_current();
	if (process == NULL) return false;
	return process_terminate(process, vmm_fault_exit_code(kind));
}

bool vmm_query(struct address_space* space, void* addr, struct vmm_info* out_info) {
	struct vmm_transfer_guard guard = {0};
	bool                      found;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !space || !addr || !out_info) return false;
	vmm_transfer_guard_acquire(&guard);
	found = vmm_query_guarded(&guard, space, addr, out_info);
	vmm_transfer_guard_release(&guard);
	return found;
}

bool vmm_query_guarded(const struct vmm_transfer_guard* guard, struct address_space* space, void* addr,
                       struct vmm_info* out_info) {
	struct memory_region* region;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (guard == NULL || !guard->active || !initialized || !space || !addr || !out_info) return false;
	region = memory_region_find_containing(space, (uintptr_t)addr);
	if (region) memory_region_fill_info(region, out_info);
	return region != NULL;
}

bool vmm_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info) {
	struct memory_region* region;
	struct irq_state      state;

	if (out_info) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !space || id == VMM_ID_INVALID || !out_info) return false;
	state  = spinlock_lock_irqsave(&vmm_lock);
	region = memory_region_find_by_id(space, id);
	if (region) memory_region_fill_info(region, out_info);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return region != NULL;
}

size_t vmm_count(struct address_space* space) {
	size_t           count;
	struct irq_state state;

	if (!space) return 0u;
	state = spinlock_lock_irqsave(&vmm_lock);
	count = memory_region_count(space);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	return count;
}

bool vmm_user_address_space_init(struct address_space* space) {
	size_t                   page_count;
	struct hal_address_space hal_space;

	if (!initialized || !space) return false;
	page_count = MM_USER_VMM_SIZE / PMM_PAGE_SIZE;
	if (page_count == 0) return false;
	if (!hal_paging_space_create(&hal_space)) return false;
	if (!address_space_init(space, (uintptr_t)MM_USER_VMM_BASE, page_count)) {
		hal_paging_space_destroy(&hal_space);
		return false;
	}
	space->hal_space = hal_space;
	return true;
}

void vmm_address_space_deinit(struct address_space* space) {
	struct irq_state         state;
	struct hal_address_space hal_space;

	if (!space) return;
	state = spinlock_lock_irqsave(&vmm_lock);
	(void)region_pager_unmap_space(space);
	memory_region_destroy_all(space);
	spinlock_unlock_irqrestore(&vmm_lock, state);
	hal_space = space->hal_space;
	address_space_deinit(space);
	if (space != address_space_kernel()) hal_paging_space_destroy(&hal_space);
}
