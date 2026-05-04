#include <base/heap.h>
#include <core/pmm.h>
#include <pthread.h>

static pthread_mutex_t test_kheap_mutex = PTHREAD_MUTEX_INITIALIZER;

size_t heap_page_size(void) {
	return PMM_PAGE_SIZE;
}

void heap_lock(void) {
	(void)pthread_mutex_lock(&test_kheap_mutex);
}

void heap_unlock(void) {
	(void)pthread_mutex_unlock(&test_kheap_mutex);
}
