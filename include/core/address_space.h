#pragma once

#include <core/mapping.h>
#include <core/spinlock.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory;
struct mapping;

/* Parameters for projecting one complete Memory into an AddressSpace. */
struct address_space_mapping_request {
	struct memory*   memory;
	uintptr_t        address;
	size_t           alignment;
	size_t           guard_before;
	size_t           guard_after;
	mapping_access_t access;
};

/* Hardware-fault classification passed to the common AddressSpace dispatcher. */
enum address_space_fault_kind {
	ADDRESS_SPACE_FAULT_NOT_PRESENT = 0,
	ADDRESS_SPACE_FAULT_PROTECTION,
	ADDRESS_SPACE_FAULT_INVALID,
	ADDRESS_SPACE_FAULT_UNCLASSIFIED,
};

/* One structurally-owned CPU virtual address space and its Mapping registry. */
struct address_space {
	uintptr_t                base;
	uintptr_t                end;
	struct hal_paging_space* hal;
	struct spinlock          lock;
	struct mapping*          mapping_root;
	struct mapping*          mapping_first;
	struct mapping*          mapping_last;
	size_t                   mapping_count;
	bool                     destroying;
};

/* Initialize AddressSpace support and the kernel AddressSpace. */
bool address_space_init(void);

/* Return the kernel AddressSpace. */
struct address_space* address_space_kernel(void);

/* Initialize a process AddressSpace. */
bool address_space_create_process(struct address_space* space);

/* Destroy an AddressSpace and detach all of its Mappings. */
void address_space_destroy(struct address_space* space);

/* Return whether an AddressSpace is initialized and usable. */
bool address_space_is_initialized(const struct address_space* space);

/* Activate an AddressSpace on the current CPU. */
bool address_space_activate(struct address_space* space);

/* Return the AddressSpace hardware paging handle. */
struct hal_paging_space* address_space_hal(struct address_space* space);

/* Lazily project one complete Memory and return a retained Mapping. */
bool address_space_map(struct address_space* space, const struct address_space_mapping_request* request,
                       struct mapping** out_mapping);

/* Remove an active Mapping from its owning AddressSpace. */
bool address_space_unmap(struct address_space* space, struct mapping* mapping);

/* Atomically change the access applied to present and future translations. */
bool address_space_protect(struct address_space* space, struct mapping* mapping, mapping_access_t access);

/* Materialize and project an aligned Mapping-relative byte range. */
bool address_space_prefault(struct address_space* space, struct mapping* mapping, size_t offset, size_t size);

/* Resolve one eligible not-present access in an AddressSpace. */
bool address_space_resolve_fault(struct address_space* space, uintptr_t address, mapping_access_t access);

/* Return the number of active Mappings in an AddressSpace. */
size_t address_space_mapping_count(struct address_space* space);

/* Return whether Mapping is currently owned by AddressSpace. */
bool address_space_contains_mapping(struct address_space* space, const struct mapping* mapping);

/* Resolve or dispatch a fault from the current execution context. */
bool address_space_handle_current_fault(uintptr_t address, enum address_space_fault_kind kind, mapping_access_t access,
                                        bool user_mode);
