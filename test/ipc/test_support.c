#include "test_support.h"

#include <base/heap.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <stdbool.h>

#define IPC_TEST_HEAP_SIZE KiB(128)

static uint8_t ipc_test_heap[IPC_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  ipc_test_heap_offset;
static bool    ipc_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	if (page_count > SIZE_MAX / PMM_PAGE_SIZE) return false;
	bytes = page_count * PMM_PAGE_SIZE;

	for (;;) {
		offset = __atomic_load_n(&ipc_test_heap_offset, __ATOMIC_ACQUIRE);
		if (offset > IPC_TEST_HEAP_SIZE || bytes > IPC_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&ipc_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = ipc_test_heap + offset;
			return true;
		}
	}
}

void ipc_test_init_heap(void) {
	if (ipc_test_heap_initialized) return;
	ipc_test_heap_offset = 0u;
	cr_assert(heap_init(), "heap_init failed");
	ipc_test_heap_initialized = true;
}
