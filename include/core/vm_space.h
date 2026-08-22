#pragma once

#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One live mapping of a memory object into an address space. */
struct vm_mapping {
	struct memory_object* memory;
	uintptr_t             base;
	size_t                page_count;
	size_t                memory_page_offset;
	vmm_id_t              id;
	size_t                guard_pages;
	vmm_prot_t            prot;
};

/* A virtual address space and its live mappings. */
struct address_space {
	uintptr_t                base;
	uintptr_t                end;
	struct hal_address_space hal;
	struct spinlock          lock;
	struct vm_mapping*       mappings;
	uintptr_t                mappings_phys;
	size_t                   mapping_count;
	size_t                   mapping_capacity;
	uint64_t                 next_mapping_id;
};

/* Parameters for mapping a memory object range. */
struct vm_map_request {
	struct memory_object* memory;
	size_t                memory_page_offset;
	size_t                page_count;
	uintptr_t             requested_base;
	size_t                align_pages;
	size_t                guard_pages;
	vmm_prot_t            prot;
};

enum vmm_fault_kind {
	VMM_FAULT_NOT_PRESENT = 0,
	VMM_FAULT_PROTECTION,
	VMM_FAULT_INVALID,
	VMM_FAULT_UNCLASSIFIED,
};

enum vmm_fault_access {
	VMM_FAULT_ACCESS_UNKNOWN = 0,
	VMM_FAULT_ACCESS_READ,
	VMM_FAULT_ACCESS_WRITE,
	VMM_FAULT_ACCESS_EXEC,
};

/* Initialize virtual memory and the kernel address space. */
bool vm_init(void);

/* Return the kernel address space. */
struct address_space* vm_space_kernel(void);

/* Initialize a user address space. */
bool vm_space_create_user(struct address_space* space);

/* Destroy an address space and release its mappings. */
void vm_space_destroy(struct address_space* space);

/* Return whether an address space is initialized. */
bool vm_space_is_initialized(const struct address_space* space);

/* Return an address space's hardware paging handle. */
struct hal_address_space* vm_space_hal(struct address_space* space);

/* Activate an address space on the current CPU. */
bool vm_space_activate(struct address_space* space);

/* Map a memory object range into an address space. */
bool vm_space_map(struct address_space* space, const struct vm_map_request* request, vmm_id_t* out_id, void** out_base);

/* Remove a mapping from an address space. */
bool vm_space_unmap(struct address_space* space, vmm_id_t id);

/* Change a mapping's protections. */
bool vm_space_protect(struct address_space* space, vmm_id_t id, vmm_prot_t prot);

/* Materialize and map a requested page range immediately. */
bool vm_space_prefault(struct address_space* space, vmm_id_t id, size_t first_page, size_t page_count);

/* Resolve one eligible page fault in an address space. */
bool vm_space_resolve_page_fault(struct address_space* space, uintptr_t address, enum vmm_fault_access access);

/* Query the mapping containing an address. */
bool vm_space_query(struct address_space* space, uintptr_t address, struct vmm_info* out_info);

/* Query a mapping by identifier. */
bool vm_space_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info);

/* Query a mapping by its sorted-vector index. */
bool vm_space_query_at(struct address_space* space, size_t index, struct vmm_info* out_info);

/* Return the number of live mappings in an address space. */
size_t vm_space_mapping_count(struct address_space* space);

/* Resolve or dispatch a page fault from the current execution context. */
bool vm_handle_current_page_fault(uintptr_t address, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                  bool user_mode);
