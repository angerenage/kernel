#include <hal/cache.h>

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	hal_cache_sync_executable_range(address, size);
}
