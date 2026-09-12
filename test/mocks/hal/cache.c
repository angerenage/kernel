#include <hal/cache.h>

static size_t executable_sync_count;
static size_t dma_device_sync_count;
static size_t dma_cpu_sync_count;
static size_t dma_device_sync_bytes;
static size_t dma_cpu_sync_bytes;

bool hal_cache_sync_for_device(void* address, size_t size) {
	if (address == NULL && size != 0u) return false;
	dma_device_sync_count++;
	dma_device_sync_bytes += size;
	return true;
}

bool hal_cache_sync_for_cpu(void* address, size_t size) {
	if (address == NULL && size != 0u) return false;
	dma_cpu_sync_count++;
	dma_cpu_sync_bytes += size;
	return true;
}

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	executable_sync_count++;
}

void mock_cache_reset(void) {
	executable_sync_count = 0u;
	dma_device_sync_count = 0u;
	dma_cpu_sync_count    = 0u;
	dma_device_sync_bytes = 0u;
	dma_cpu_sync_bytes    = 0u;
}

size_t mock_cache_executable_sync_count(void) {
	return executable_sync_count;
}

size_t mock_cache_dma_device_sync_count(void) {
	return dma_device_sync_count;
}

size_t mock_cache_dma_cpu_sync_count(void) {
	return dma_cpu_sync_count;
}

size_t mock_cache_dma_device_sync_bytes(void) {
	return dma_device_sync_bytes;
}

size_t mock_cache_dma_cpu_sync_bytes(void) {
	return dma_cpu_sync_bytes;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	hal_cache_sync_executable_range(address, size);
}
