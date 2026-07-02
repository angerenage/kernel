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

/* Map a boot module capability into the caller's address space. */
syscall_status_t module_map(cap_id_t module_cap, uintptr_t* out_mapped_base);
