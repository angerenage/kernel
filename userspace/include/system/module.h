#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resolve a boot module by name into a process. target == PROCESS_PID_INVALID maps into the caller. */
bool module_resolve(const char* name, size_t name_length, process_id_t target, cap_id_t* out_module_cap);

/* Map a boot module capability into the caller's address space. */
bool module_map(cap_id_t module_cap, uintptr_t* out_mapped_base);
