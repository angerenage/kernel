#pragma once

#include <base/cap.h>
#include <base/module.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Resolve a boot module by name and grant its capability to the caller. */
syscall_status_t module_resolve(const char* name, size_t name_length, struct module_query_response* out_module);

/* Read descriptive metadata through a boot-module capability. */
syscall_status_t module_get_info(cap_id_t module_cap, struct module_info_response* out_info);

/* Map a boot module and return its mapping capability and initial mapping snapshot. */
syscall_status_t module_map(cap_id_t module_cap, struct module_map_response* out_mapping);

/* Copy an exact byte range from a boot module. */
syscall_status_t module_read(cap_id_t module_cap, uint64_t offset, void* buffer, size_t size);
