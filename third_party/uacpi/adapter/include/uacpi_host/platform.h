#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Configure the process capabilities used by uACPI operation-region access. */
bool uacpi_host_platform_initialize(cap_id_t memory_allocator_cap, cap_id_t io_ports_cap);

/* Release any operation-region mappings that survived uACPI teardown. */
void uacpi_host_platform_deinitialize(void);
