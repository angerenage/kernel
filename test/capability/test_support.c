#include "test_support.h"

#include <core/process.h>
#include <core/vm_space.h>

#define CAP_TEST_HEAP_SIZE ((size_t)8u * 1024u * 1024u)
#define CAP_TEST_TARGET_COUNT 128u

static uint8_t cap_test_heap[CAP_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  cap_test_heap_offset;
static bool    cap_test_heap_initialized;
static bool    cap_test_targets_initialized;

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

static void cap_test_init_targets(void) {
	if (cap_test_targets_initialized) return;

	cr_assert(vm_init(), "vm_init failed");
	for (process_id_t expected = 1u; expected <= CAP_TEST_TARGET_COUNT; expected++) {
		struct process* process = NULL;
		cr_assert_eq(process_create(&process, NULL),
		             PROCESS_OK,
		             "failed to create capability target %llu",
		             (unsigned long long)expected);
		cr_assert_not_null(process);
		cr_assert_eq(process_pid(process), expected, "capability target pid sequence changed");
	}
	cap_test_targets_initialized = true;
}

void cap_test_setup(void) {
	if (!cap_test_heap_initialized) {
		cap_test_heap_offset = 0u;
		cr_assert(heap_init(), "heap_init failed");
		cap_test_heap_initialized = true;
	}
	capability_init();
	cap_test_init_targets();
}
