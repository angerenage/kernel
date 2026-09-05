#include <base/heap.h>
#include <base/vmm.h>
#include <core/pmm.h>
#include <pthread.h>

static pthread_mutex_t test_heap_mutex = PTHREAD_MUTEX_INITIALIZER;

size_t heap_page_size(void) {
	return VMM_PAGE_SIZE;
}

void heap_lock(void) {
	(void)pthread_mutex_lock(&test_heap_mutex);
}

void heap_unlock(void) {
	(void)pthread_mutex_unlock(&test_heap_mutex);
}
