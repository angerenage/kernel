#pragma once

#include <core/address_space.h>
#include <core/memory.h>

/* Stable Mapping metadata with intrusive ordered-tree and list links. */
struct mapping {
	struct address_space* owner;
	struct memory*        memory;
	uintptr_t             address;
	size_t                size;
	size_t                guard_before;
	size_t                guard_after;
	enum memory_type      memory_type;
	memory_access_t       access;
	uint64_t              reference_count;
	struct mapping*       parent;
	struct mapping*       left;
	struct mapping*       right;
	struct mapping*       previous;
	struct mapping*       next;
	int                   height;
};

/* Find the usable Mapping containing address while the AddressSpace is locked. */
struct mapping* address_space_find_mapping_locked(struct address_space* space, uintptr_t address);

/* Convert Mapping access into exact paging flags for a PROCESS AddressSpace. */
uint64_t address_space_paging_flags(const struct address_space* space, memory_access_t access);

/* Resolve one minimum translation unit in a locked PROCESS AddressSpace. */
bool address_space_resolve_process_locked(struct address_space* space, struct mapping* mapping, size_t offset);
