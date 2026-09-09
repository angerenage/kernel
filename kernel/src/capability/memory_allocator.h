#pragma once

#include <base/cap.h>
#include <base/process.h>

/* Initialize the root MemoryAllocator policy. */
bool kernel_memory_allocator_init(void);

/* Grant the root MemoryAllocator to one process. */
cap_id_t kernel_memory_allocator_grant_root(process_id_t recipient);
