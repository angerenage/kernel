#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Release CPU cache ownership before a normal-memory range is used for DMA. */
bool hal_cache_sync_for_device(void* address, size_t size);

/* Acquire CPU cache ownership after DMA activity on a normal-memory range has completed. */
bool hal_cache_sync_for_cpu(void* address, size_t size);

/* Make written bytes visible to instruction fetch on the current CPU. */
void hal_cache_sync_executable_range(void* address, size_t size);

/* Make written bytes visible to instruction fetch on every online CPU. */
void hal_cache_sync_executable_range_all_cpus(void* address, size_t size);
