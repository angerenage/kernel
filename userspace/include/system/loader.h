#pragma once

#include <base/cap.h>
#include <base/loader.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Load an ELF boot module into a new process without starting its main thread. */
syscall_status_t loader_load(cap_id_t loader_cap, cap_id_t module_cap, struct loader_load_response* out_response);
