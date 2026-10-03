#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Expose one boot module as a Blob, taking ownership of module_cap on success. */
syscall_status_t module_blob_create(cap_id_t module_cap, size_t module_size, cap_id_t* out_blob_cap);
