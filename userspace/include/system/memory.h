#pragma once

#include <base/address_space.h>
#include <base/cap.h>
#include <base/memory.h>
#include <base/syscall.h>
#include <base/vmm.h>
#include <stddef.h>

/* Create a logical memory object and return its capability. */
syscall_status_t memory_create(const struct memory_create_params* params, cap_id_t* out_memory_cap);

/* Read immutable logical metadata through a memory capability. */
syscall_status_t memory_get_info(cap_id_t memory_cap, struct memory_info* out_info);

/* Copy memory object contents into the caller's address space. */
syscall_status_t memory_read(cap_id_t memory_cap, size_t offset, void* destination, size_t size);

/* Copy caller contents into a memory object. */
syscall_status_t memory_write(cap_id_t memory_cap, size_t offset, const void* source, size_t size);

/* Map a memory object range and return its control capability and metadata. */
syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t memory_cap,
                                   const struct memory_map_params* params, struct address_space_map_result* out_result);

/* Read a live mapping's persistent metadata. */
syscall_status_t mapping_get_info(cap_id_t mapping_cap, struct vmm_info* out_info);

/* Change a mapping's protection within its capability authority. */
syscall_status_t mapping_protect(cap_id_t mapping_cap, vmm_prot_t prot);

/* Explicitly destroy a mapping through its control capability. */
syscall_status_t mapping_unmap(cap_id_t mapping_cap);
