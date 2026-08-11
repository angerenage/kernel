#include "test_support.h"

#define CAP_TEST_HEAP_SIZE ((size_t)128u * 1024u)

static uint8_t cap_test_heap[CAP_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  cap_test_heap_offset;
static bool    cap_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	bytes     = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&cap_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > CAP_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&cap_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = cap_test_heap + offset;
			return true;
		}
	}
}

void cap_test_setup(void) {
	if (!cap_test_heap_initialized) {
		cap_test_heap_offset = 0u;
		cr_assert(heap_init(), "heap_init failed");
		cap_test_heap_initialized = true;
	}
	capability_init();
}
