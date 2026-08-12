#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates or reuses a capability for the boot module at the given index. When non-NULL, out_created reports whether
 * a new grant was published. Returns CAP_ID_INVALID on failure. */
cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t recipient, bool* out_created);

/* Resolve an authorized boot-module capability to its immutable kernel descriptor. */
syscall_result_t kernel_capability_boot_module_get(cap_id_t module_cap, process_id_t caller,
                                                   cap_rights_t                      required_rights,
                                                   const struct kernel_boot_module** out_module);
