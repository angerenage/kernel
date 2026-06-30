#pragma once

#include <base/address_space.h>
#include <base/cap.h>
#include <base/memory.h>
#include <base/syscall.h>
#include <base/vmm.h>
#include <stddef.h>
#include <stdint.h>

/* Allocate physical memory and return an allocation capability. */
syscall_status_t memory_allocate(size_t page_count, vmm_prot_t prot, enum vmm_kind kind, cap_id_t* out_allocation_cap);

/* Copy src bytes into an allocation through its capability. */
syscall_status_t allocation_write(cap_id_t allocation_cap, uintptr_t dst_offset, const void* src, size_t size);

/* Copy bytes out of an allocation through its capability. */
syscall_status_t allocation_read(cap_id_t allocation_cap, uintptr_t src_offset, void* dst, size_t size);

/* Release an allocation by destroying its capability. */
syscall_status_t allocation_free(cap_id_t allocation_cap);

/* Map an allocation into an address space at an automatic address. */
syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t allocation_cap, cap_id_t* out_mapping_cap);

/* Map an allocation into an address space at a specific page-aligned address. */
syscall_status_t address_space_map_at(cap_id_t address_space_cap, cap_id_t allocation_cap, uintptr_t address,
                                      cap_id_t* out_mapping_cap);

/* Query an allocation tracked by an address space. */
syscall_status_t address_space_query(cap_id_t address_space_cap, vmm_id_t id, struct vmm_info* out_info);

/* Read a mapping's base address, size, protection, kind, and state. */
syscall_status_t mapping_get_info(cap_id_t mapping_cap, struct vmm_info* out_info);

/* Change a mapping's protection bits. */
syscall_status_t mapping_protect(cap_id_t mapping_cap, vmm_prot_t prot);

/* Unmap a mapping through its mapping capability. */
syscall_status_t mapping_unmap(cap_id_t mapping_cap);
