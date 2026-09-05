#pragma once

#include <stddef.h>

/* Make written bytes visible to instruction fetch on the current CPU. */
void hal_cache_sync_executable_range(void* address, size_t size);

/* Make written bytes visible to instruction fetch on every online CPU. */
void hal_cache_sync_executable_range_all_cpus(void* address, size_t size);
