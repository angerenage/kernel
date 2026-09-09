#pragma once

#include <base/address_space.h>
#include <base/memory.h>
#include <base/syscall.h>

/* Read the immutable policy of a MemoryAllocator. */
syscall_status_t memory_allocator_info(cap_id_t allocator_cap, struct memory_allocator_info* out_info);

/* Derive a child MemoryAllocator using a complete variable-sized request. */
syscall_status_t memory_allocator_derive(cap_id_t allocator_cap, const struct memory_allocator_derive_request* request,
                                         size_t request_size, cap_id_t* out_allocator_cap);

/* Allocate sparse NORMAL Memory of an exact byte size. */
syscall_status_t memory_allocator_alloc(cap_id_t allocator_cap, size_t size, cap_id_t* out_memory_cap);

/* Claim exclusive ownership of one physical byte range. */
syscall_status_t memory_allocator_claim_physical(cap_id_t allocator_cap, uintptr_t physical_address, size_t size,
                                                 enum memory_type memory_type, cap_id_t* out_memory_cap);

/* Read immutable logical Memory information. */
syscall_status_t memory_info(cap_id_t memory_cap, struct memory_info* out_info);

/* Create a capability-tree child Memory slice. */
syscall_status_t memory_slice(cap_id_t memory_cap, size_t offset, size_t size, cap_id_t* out_memory_cap);

/* Read the public geometry of a process AddressSpace. */
syscall_status_t address_space_info(cap_id_t address_space_cap, struct address_space_info* out_info);

/* Map one complete Memory into a process AddressSpace. */
syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t memory_cap, memory_access_t access,
                                   uintptr_t address, size_t alignment, size_t guard_before, size_t guard_after,
                                   struct address_space_map_response* out_response);

/* Read current Mapping information. */
syscall_status_t mapping_info(cap_id_t mapping_cap, struct mapping_info* out_info);

/* Change a Mapping's logical access within its authority. */
syscall_status_t mapping_protect(cap_id_t mapping_cap, memory_access_t access);

/* Explicitly destroy a Mapping and all grants to it. */
syscall_status_t mapping_unmap(cap_id_t mapping_cap);
