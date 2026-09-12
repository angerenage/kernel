#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/process.h>

struct address_space;
struct cap_object;

/* Grant recipient a capability for process' address space. */
cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights);

/* Grant recipient a capability for one retained DEVICE AddressSpace. */
cap_id_t kernel_device_address_space_grant(struct address_space* space, process_id_t recipient, cap_rights_t rights);

/* Resolve a DEVICE AddressSpace capability and retain its routing object for the caller. */
syscall_result_t kernel_device_address_space_acquire(cap_id_t address_space_cap, process_id_t caller,
                                                     cap_rights_t required_rights, struct cap_object** out_object,
                                                     struct address_space** out_space, cap_rights_t* out_rights);
