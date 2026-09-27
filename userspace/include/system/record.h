#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Read the number of records exposed by a record access capability. */
syscall_status_t record_count(cap_id_t record_cap, uint64_t* out_count);

/* Read one client-typed record from a record access capability. */
syscall_status_t record_get(cap_id_t record_cap, uint64_t index, void* out_record, size_t record_size);
