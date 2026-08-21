#pragma once

#include <core/memory_object.h>
#include <core/vm_space.h>

/* Find the mapping containing an address while the address-space lock is held. */
struct vm_mapping* vm_mapping_find_locked(struct address_space* space, uintptr_t address);

/* Find a mapping by identifier while the address-space lock is held. */
struct vm_mapping* vm_mapping_find_id_locked(struct address_space* space, vmm_id_t id);

/* Convert mapping protections to hardware page flags for an address space. */
uint64_t vm_mapping_hal_flags(const struct address_space* space, vmm_prot_t prot);
