#pragma once

#include <base/vmm.h>
#include <stdbool.h>

struct address_space;

enum vmm_fault_kind {
	VMM_FAULT_NOT_PRESENT = 0,
	VMM_FAULT_PROTECTION,
	VMM_FAULT_INVALID,
};

enum vmm_fault_access {
	VMM_FAULT_ACCESS_UNKNOWN = 0,
	VMM_FAULT_ACCESS_READ,
	VMM_FAULT_ACCESS_WRITE,
	VMM_FAULT_ACCESS_EXEC,
};

/* Initialize the paging backend and reserve the kernel's managed virtual window. Re-initializing resets metadata. */
bool vmm_init(void);

/*
 * Creates a tracked virtual allocation in an explicit address space.
 * - With VMM_MAP_LAZY: reserve only, state starts RESERVED.
 * - Without VMM_MAP_LAZY: pages are allocated + mapped immediately.
 * - Lazy allocations may move through RESERVED -> PARTIAL -> MAPPED.
 * - STACK allocations reserve extra guard pages below the returned base.
 */
bool vmm_alloc(struct address_space* space, const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base);

/* Create a tracked virtual allocation whose usable base is exactly base. */
bool vmm_alloc_at(struct address_space* space, void* base, const struct vmm_alloc_params* params, vmm_id_t* out_id);

/* Destroy allocation metadata and backing owned by an explicit address space. */
bool vmm_free(struct address_space* space, vmm_id_t id);

/* Materialize all pages in an explicit address-space allocation. */
bool vmm_map(struct address_space* space, vmm_id_t id);

/* Remove all live mappings from an explicit address-space allocation. */
bool vmm_unmap(struct address_space* space, vmm_id_t id, bool release_phys);

/* Change protection flags on an explicit address-space allocation. */
bool vmm_protect(struct address_space* space, vmm_id_t id, vmm_prot_t new_prot);

/* Resolve a lazy fault inside an explicit address space. */
bool vmm_resolve_page_fault(struct address_space* space, uintptr_t addr);

/* Resolve or dispatch a current page fault. Kernel faults return false for architecture fatal handling. */
bool vmm_handle_current_page_fault(uintptr_t addr, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                   bool user_mode);

/* Query the tracked allocation owning an address inside an explicit address space. */
bool vmm_query(struct address_space* space, void* addr, struct vmm_info* out_info);

/* Query an explicit address-space allocation by id. */
bool vmm_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info);

/* Number of live tracked allocations in an explicit address space. */
size_t vmm_count(struct address_space* space);

/* Initialize a user address space over the configured user virtual range. */
bool vmm_user_address_space_init(struct address_space* space);

/* Release all VMM metadata/backing and the virtual-range allocator for an address space. */
void vmm_address_space_deinit(struct address_space* space);
