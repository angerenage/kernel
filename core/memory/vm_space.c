#include <base/math.h>
#include <base/process.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <hal/hcf.h>
#include <string.h>

#include "vm_space_internal.h"

static struct address_space kernel_space = {
	.lock = SPINLOCK_INIT_CLASS("kernel_address_space", SPINLOCK_ORDER_VADDR,
                                SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION),
};
static bool initialized;

static bool prot_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0u;
}
static bool space_is_kernel(const struct address_space* space) {
	return space == &kernel_space;
}
static uintptr_t mapping_reserved_start(const struct vm_mapping* mapping) {
	return mapping->base - mapping->guard_pages * (uintptr_t)PMM_PAGE_SIZE;
}
static uintptr_t mapping_reserved_end(const struct vm_mapping* mapping) {
	return mapping->base + mapping->page_count * (uintptr_t)PMM_PAGE_SIZE;
}
static void fill_info(const struct vm_mapping* mapping, struct vmm_info* info) {
	*info = (struct vmm_info){
		.id                 = mapping->id,
		.base               = (void*)mapping->base,
		.page_count         = mapping->page_count,
		.memory_page_offset = mapping->memory_page_offset,
		.guard_pages        = mapping->guard_pages,
		.prot               = mapping->prot,
	};
}

uint64_t vm_mapping_hal_flags(const struct address_space* space, vmm_prot_t prot) {
	uint64_t flags = 0u;
	if ((prot & VMM_PROT_WRITE) != 0u) flags |= HAL_PAGE_WRITE;
	if ((prot & VMM_PROT_EXEC) != 0u) flags |= HAL_PAGE_EXEC;
	if ((prot & VMM_PROT_GLOBAL) != 0u) flags |= HAL_PAGE_GLOBAL;
	if ((prot & VMM_PROT_NO_CACHE) != 0u) flags |= HAL_PAGE_NO_CACHE;
	if (!space_is_kernel(space)) flags |= HAL_PAGE_USER;
	return flags;
}

bool vm_space_is_initialized(const struct address_space* space) {
	return space != NULL && space->end > space->base;
}

struct address_space* vm_space_kernel(void) {
	return &kernel_space;
}

struct hal_address_space* vm_space_hal(struct address_space* space) {
	return vm_space_is_initialized(space) ? &space->hal : NULL;
}

bool vm_space_activate(struct address_space* space) {
	return vm_space_is_initialized(space) && hal_paging_activate(&space->hal);
}

static bool space_init(struct address_space* space, uintptr_t base, size_t bytes, const struct hal_address_space* hal) {
	uint64_t end;
	if (space == NULL || hal == NULL || bytes == 0u || (base & (PMM_PAGE_SIZE - 1u)) != 0u ||
	    (bytes & (PMM_PAGE_SIZE - 1u)) != 0u || add_overflow_u64(base, bytes, &end))
		return false;
	memset(space, 0, sizeof(*space));
	space->base            = base;
	space->end             = (uintptr_t)end;
	space->hal             = *hal;
	space->next_mapping_id = 1u;
	spinlock_init_class(&space->lock,
	                    space_is_kernel(space) ? "kernel_address_space" : "user_address_space",
	                    SPINLOCK_ORDER_VADDR,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	return true;
}

bool vm_init(void) {
	struct hal_address_space* hal;
	initialized = false;
	if (vm_space_is_initialized(&kernel_space)) vm_space_destroy(&kernel_space);
	if (!hal_paging_init() || (hal = hal_paging_kernel_space()) == NULL ||
	    !space_init(&kernel_space, MM_KERNEL_VMM_BASE, MM_KERNEL_VMM_SIZE, hal))
		return false;
	initialized = true;
	return true;
}

bool vm_space_create_user(struct address_space* space) {
	struct hal_address_space hal;
	if (!initialized || space == NULL || space == &kernel_space || !hal_paging_space_create(&hal)) return false;
	if (!space_init(space, MM_USER_VMM_BASE, MM_USER_VMM_SIZE, &hal)) {
		hal_paging_space_destroy(&hal);
		return false;
	}
	return true;
}

static size_t vector_pages(size_t capacity) {
	if (capacity == 0u) return 0u;
	return (capacity * sizeof(struct vm_mapping) + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;
}

static bool vector_grow(struct address_space* space) {
	size_t             old_pages = vector_pages(space->mapping_capacity);
	size_t             new_pages = old_pages == 0u ? 1u : old_pages * 2u;
	uintptr_t          phys      = 0u;
	struct vm_mapping* mappings;
	if (new_pages < old_pages || !pmm_alloc_pages(new_pages, &phys)) return false;
	mappings = (struct vm_mapping*)(uintptr_t)(phys + boot_info.direct_map_offset);
	memset(mappings, 0, new_pages * PMM_PAGE_SIZE);
	if (space->mapping_count != 0u) memcpy(mappings, space->mappings, space->mapping_count * sizeof(*mappings));
	if (old_pages != 0u) (void)pmm_free_pages(space->mappings_phys, old_pages);
	space->mappings         = mappings;
	space->mappings_phys    = phys;
	space->mapping_capacity = new_pages * PMM_PAGE_SIZE / sizeof(*mappings);
	return true;
}

static size_t lower_bound_reserved(const struct address_space* space, uintptr_t reserved_start) {
	size_t lo = 0u, hi = space->mapping_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2u;
		if (mapping_reserved_start(&space->mappings[mid]) < reserved_start) lo = mid + 1u;
		else hi = mid;
	}
	return lo;
}

struct vm_mapping* vm_mapping_find_locked(struct address_space* space, uintptr_t address) {
	size_t lo = 0u, hi;
	if (space == NULL) return NULL;
	hi = space->mapping_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2u;
		if (space->mappings[mid].base <= address) lo = mid + 1u;
		else hi = mid;
	}
	if (lo == 0u) return NULL;
	struct vm_mapping* mapping = &space->mappings[lo - 1u];
	return address < mapping_reserved_end(mapping) ? mapping : NULL;
}

struct vm_mapping* vm_mapping_find_id_locked(struct address_space* space, vmm_id_t id) {
	if (space == NULL || id == VMM_ID_INVALID) return NULL;
	for (size_t i = 0u; i < space->mapping_count; i++)
		if (space->mappings[i].id == id) return &space->mappings[i];
	return NULL;
}

static bool mapping_span(const struct vm_map_request* request, size_t* out_guard_bytes, size_t* out_usable_bytes) {
	return !mul_overflow_size(request->guard_pages, PMM_PAGE_SIZE, out_guard_bytes) &&
	       !mul_overflow_size(request->page_count, PMM_PAGE_SIZE, out_usable_bytes);
}

static bool find_placement(const struct address_space* space, const struct vm_map_request* request, uintptr_t* out_base,
                           size_t* out_position) {
	size_t    guard_bytes, usable_bytes, align_bytes;
	uintptr_t gap_start = space->base;
	if (!mapping_span(request, &guard_bytes, &usable_bytes) ||
	    mul_overflow_size(request->align_pages == 0u ? 1u : request->align_pages, PMM_PAGE_SIZE, &align_bytes))
		return false;
	for (size_t i = 0u; i <= space->mapping_count; i++) {
		uintptr_t gap_end = i == space->mapping_count ? space->end : mapping_reserved_start(&space->mappings[i]);
		uint64_t  before_align, usable, reserved_end;
		if (!add_overflow_u64(gap_start, guard_bytes, &before_align) &&
		    align_up_u64(before_align, align_bytes, &usable) && usable >= guard_bytes &&
		    !add_overflow_u64(usable, usable_bytes, &reserved_end) && reserved_end <= gap_end) {
			*out_base     = (uintptr_t)usable;
			*out_position = i;
			return true;
		}
		if (i < space->mapping_count) gap_start = mapping_reserved_end(&space->mappings[i]);
	}
	return false;
}

bool vm_space_map(struct address_space* space, const struct vm_map_request* request, vmm_id_t* out_id,
                  void** out_base) {
	struct irq_state state;
	size_t           object_end, guard_bytes, usable_bytes, align_pages, align_bytes, position;
	uintptr_t        base, reserved_start, reserved_end;
	if (out_id != NULL) *out_id = VMM_ID_INVALID;
	if (out_base != NULL) *out_base = NULL;
	if (!initialized || !vm_space_is_initialized(space) || request == NULL || request->memory == NULL ||
	    request->page_count == 0u || (out_id == NULL && out_base == NULL) || !prot_valid(request->prot) ||
	    add_overflow_size(request->memory_page_offset, request->page_count, &object_end) ||
	    object_end > memory_object_page_count(request->memory) || !mapping_span(request, &guard_bytes, &usable_bytes))
		return false;
	align_pages = request->align_pages == 0u ? 1u : request->align_pages;
	if ((align_pages & (align_pages - 1u)) != 0u || mul_overflow_size(align_pages, PMM_PAGE_SIZE, &align_bytes))
		return false;
	if (space_is_kernel(space)) {
		/* Kernel mappings never acquire user accessibility. */
	}
	else if ((request->prot & VMM_PROT_GLOBAL) != 0u) return false;
	state = spinlock_lock_irqsave(&space->lock);
	if (request->requested_base == 0u) {
		if (!find_placement(space, request, &base, &position)) goto fail;
		reserved_start = base - guard_bytes;
		reserved_end   = base + usable_bytes;
	}
	else {
		base = request->requested_base;
		if ((base & (PMM_PAGE_SIZE - 1u)) != 0u || (base & (align_bytes - 1u)) != 0u || base < guard_bytes) goto fail;
		reserved_start = base - guard_bytes;
		if (add_overflow_u64(base, usable_bytes, (uint64_t*)&reserved_end) || reserved_start < space->base ||
		    reserved_end > space->end)
			goto fail;
		position = lower_bound_reserved(space, reserved_start);
		if ((position != 0u && mapping_reserved_end(&space->mappings[position - 1u]) > reserved_start) ||
		    (position != space->mapping_count && mapping_reserved_start(&space->mappings[position]) < reserved_end))
			goto fail;
	}
	if (space->mapping_count == space->mapping_capacity && !vector_grow(space)) goto fail;
	if (!memory_object_retain(request->memory)) goto fail;
	memmove(&space->mappings[position + 1u],
	        &space->mappings[position],
	        (space->mapping_count - position) * sizeof(*space->mappings));
	vmm_id_t id = space->next_mapping_id++;
	if (id == VMM_ID_INVALID) id = space->next_mapping_id++;
	space->mappings[position] = (struct vm_mapping){
		.memory             = request->memory,
		.base               = base,
		.page_count         = request->page_count,
		.memory_page_offset = request->memory_page_offset,
		.id                 = id,
		.guard_pages        = request->guard_pages,
		.prot               = request->prot,
	};
	space->mapping_count++;
	if (out_id != NULL) *out_id = id;
	if (out_base != NULL) *out_base = (void*)base;
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
fail:
	spinlock_unlock_irqrestore(&space->lock, state);
	return false;
}

bool vm_space_unmap(struct address_space* space, vmm_id_t id) {
	struct irq_state      state;
	struct vm_mapping*    mapping;
	size_t                index;
	struct memory_object* memory;
	if (!initialized || !vm_space_is_initialized(space) || id == VMM_ID_INVALID) return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_id_locked(space, id);
	if (mapping == NULL || !hal_paging_unmap_range(&space->hal, mapping->base, mapping->page_count)) {
		spinlock_unlock_irqrestore(&space->lock, state);
		return false;
	}
	index  = (size_t)(mapping - space->mappings);
	memory = mapping->memory;
	memmove(mapping, mapping + 1u, (space->mapping_count - index - 1u) * sizeof(*mapping));
	space->mapping_count--;
	memory_object_release(memory);
	if (space->mapping_count == 0u) {
		(void)pmm_free_pages(space->mappings_phys, vector_pages(space->mapping_capacity));
		space->mappings         = NULL;
		space->mappings_phys    = 0u;
		space->mapping_capacity = 0u;
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
}

bool vm_space_protect(struct address_space* space, vmm_id_t id, vmm_prot_t prot) {
	struct irq_state   state;
	struct vm_mapping* mapping;
	if (!initialized || !vm_space_is_initialized(space) || id == VMM_ID_INVALID || !prot_valid(prot) ||
	    (!space_is_kernel(space) && (prot & VMM_PROT_GLOBAL) != 0u))
		return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_id_locked(space, id);
	if (mapping == NULL ||
	    !hal_paging_protect_range(&space->hal, mapping->base, mapping->page_count, vm_mapping_hal_flags(space, prot))) {
		spinlock_unlock_irqrestore(&space->lock, state);
		return false;
	}
	mapping->prot = prot;
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
}

static bool access_allowed(vmm_prot_t prot, enum vmm_fault_access access) {
	switch (access) {
	case VMM_FAULT_ACCESS_READ:
		return (prot & VMM_PROT_READ) != 0u;
	case VMM_FAULT_ACCESS_WRITE:
		return (prot & VMM_PROT_WRITE) != 0u;
	case VMM_FAULT_ACCESS_EXEC:
		return (prot & VMM_PROT_EXEC) != 0u;
	default:
		return true;
	}
}

static bool resolve_locked(struct address_space* space, struct vm_mapping* mapping, size_t page) {
	uintptr_t virt = mapping->base + page * (uintptr_t)PMM_PAGE_SIZE;
	uintptr_t phys;
	if (hal_paging_query(&space->hal, virt, NULL, NULL)) return true;
	if (!memory_object_resolve_page(mapping->memory, mapping->memory_page_offset + page, &phys)) return false;
	return hal_paging_map(&space->hal, virt, phys, vm_mapping_hal_flags(space, mapping->prot));
}

bool vm_space_prefault(struct address_space* space, vmm_id_t id, size_t first_page, size_t page_count) {
	struct irq_state   state;
	struct vm_mapping* mapping;
	size_t             end;
	if (!initialized || !vm_space_is_initialized(space) || id == VMM_ID_INVALID || page_count == 0u ||
	    add_overflow_size(first_page, page_count, &end))
		return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_id_locked(space, id);
	if (mapping == NULL || end > mapping->page_count) goto fail;
	for (size_t page = first_page; page < end; page++)
		if (!resolve_locked(space, mapping, page)) goto fail;
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
fail:
	spinlock_unlock_irqrestore(&space->lock, state);
	return false;
}

bool vm_space_resolve_page_fault(struct address_space* space, uintptr_t address, enum vmm_fault_access access) {
	struct irq_state   state;
	struct vm_mapping* mapping;
	bool               ok = false;
	if (!initialized || !vm_space_is_initialized(space)) return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_locked(space, address);
	if (mapping != NULL && access_allowed(mapping->prot, access)) {
		size_t page = (address - mapping->base) / PMM_PAGE_SIZE;
		ok          = resolve_locked(space, mapping, page);
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return ok;
}

bool vm_space_query(struct address_space* space, uintptr_t address, struct vmm_info* out_info) {
	struct irq_state   state;
	struct vm_mapping* mapping;
	if (out_info != NULL) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !vm_space_is_initialized(space) || out_info == NULL) return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_locked(space, address);
	if (mapping != NULL) fill_info(mapping, out_info);
	spinlock_unlock_irqrestore(&space->lock, state);
	return mapping != NULL;
}

bool vm_space_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info) {
	struct irq_state   state;
	struct vm_mapping* mapping;
	if (out_info != NULL) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !vm_space_is_initialized(space) || out_info == NULL) return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = vm_mapping_find_id_locked(space, id);
	if (mapping != NULL) fill_info(mapping, out_info);
	spinlock_unlock_irqrestore(&space->lock, state);
	return mapping != NULL;
}

bool vm_space_query_at(struct address_space* space, size_t index, struct vmm_info* out_info) {
	struct irq_state state;
	bool             found;
	if (out_info != NULL) memset(out_info, 0, sizeof(*out_info));
	if (!initialized || !vm_space_is_initialized(space) || out_info == NULL) return false;
	state = spinlock_lock_irqsave(&space->lock);
	found = index < space->mapping_count;
	if (found) fill_info(&space->mappings[index], out_info);
	spinlock_unlock_irqrestore(&space->lock, state);
	return found;
}

size_t vm_space_mapping_count(struct address_space* space) {
	struct irq_state state;
	size_t           count;
	if (!vm_space_is_initialized(space)) return 0u;
	state = spinlock_lock_irqsave(&space->lock);
	count = space->mapping_count;
	spinlock_unlock_irqrestore(&space->lock, state);
	return count;
}

void vm_space_destroy(struct address_space* space) {
	struct irq_state         state;
	struct hal_address_space hal;
	if (!vm_space_is_initialized(space)) return;
	state = spinlock_lock_irqsave(&space->lock);
	hal   = space->hal;
	if (space_is_kernel(space)) {
		for (size_t i = 0u; i < space->mapping_count; i++)
			if (!hal_paging_unmap_range(&space->hal, space->mappings[i].base, space->mappings[i].page_count)) hcf();
	}
	else hal_paging_space_destroy(&hal);
	for (size_t i = 0u; i < space->mapping_count; i++) memory_object_release(space->mappings[i].memory);
	if (space->mapping_capacity != 0u)
		(void)pmm_free_pages(space->mappings_phys, vector_pages(space->mapping_capacity));
	space->base             = 0u;
	space->end              = 0u;
	space->hal              = (struct hal_address_space){0};
	space->mappings         = NULL;
	space->mappings_phys    = 0u;
	space->mapping_count    = 0u;
	space->mapping_capacity = 0u;
	space->next_mapping_id  = 0u;
	spinlock_unlock_irqrestore(&space->lock, state);
}

static enum vmm_fault_kind classify_fault(struct address_space* space, uintptr_t address) {
	return vm_space_is_initialized(space) &&
	               hal_paging_query(&space->hal, address & ~(uintptr_t)(PMM_PAGE_SIZE - 1u), NULL, NULL)
	           ? VMM_FAULT_PROTECTION
	           : VMM_FAULT_NOT_PRESENT;
}

bool vm_handle_current_page_fault(uintptr_t address, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                  bool user_mode) {
	struct thread*        current       = sched_current_thread();
	struct address_space* current_space = current == NULL ? NULL : current->address_space;
	if (kind == VMM_FAULT_UNCLASSIFIED) {
		kind = classify_fault(current_space, address);
		if (kind == VMM_FAULT_NOT_PRESENT && !user_mode && current_space != &kernel_space)
			kind = classify_fault(&kernel_space, address);
	}
	if (kind == VMM_FAULT_NOT_PRESENT) {
		if (current_space != NULL && vm_space_resolve_page_fault(current_space, address, access)) return true;
		if (!user_mode && current_space != &kernel_space && vm_space_resolve_page_fault(&kernel_space, address, access))
			return true;
	}
	if (!user_mode) return false;
	struct process* process = process_current();
	if (process == NULL) return false;
	uintptr_t code = kind == VMM_FAULT_PROTECTION    ? PROCESS_EXIT_MEMORY_PROTECTION
	                 : kind == VMM_FAULT_NOT_PRESENT ? PROCESS_EXIT_MEMORY_NOT_PRESENT
	                                                 : PROCESS_EXIT_MEMORY_INVALID;
	return process_terminate(process, code);
}
