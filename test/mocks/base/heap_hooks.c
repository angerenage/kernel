#include <base/heap.h>
#include <core/pmm.h>
#include <pthread.h>
#include <test_memory.h>

static pthread_mutex_t test_heap_mutex = PTHREAD_MUTEX_INITIALIZER;

size_t heap_growth_granule(void) {
	return TEST_MAPPING_GRANULE;
}

void heap_lock(void) {
	(void)pthread_mutex_lock(&test_heap_mutex);
}

void heap_unlock(void) {
	(void)pthread_mutex_unlock(&test_heap_mutex);
}
