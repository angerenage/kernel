#include <hal/cache.h>

static size_t executable_sync_count;

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	executable_sync_count++;
}

void mock_cache_reset(void) {
	executable_sync_count = 0u;
}

size_t mock_cache_executable_sync_count(void) {
	return executable_sync_count;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	hal_cache_sync_executable_range(address, size);
}
