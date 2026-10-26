#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/device_manager_control.h>
#include <stddef.h>

/* Read metadata for the device manager's active driver catalog. */
syscall_status_t device_manager_catalog_info(cap_id_t manager_cap, struct device_manager_catalog_info* out_info);

/* Atomically replace the active driver catalog from an absolute VFS path. */
syscall_status_t device_manager_update_catalog(cap_id_t manager_cap, const char* path, size_t path_size,
                                               struct device_manager_update_catalog_response* out_response);
