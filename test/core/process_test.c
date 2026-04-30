#include <core/kheap.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>

#include "../vmm/test_support.h"

#define PROCESS_TEST_ARENA_SIZE KiB(2048)
#define PROCESS_TEST_HEAP_SIZE KiB(64)

static uint8_t process_test_arena[PROCESS_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t process_test_heap[PROCESS_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  process_test_heap_offset;

bool kheap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&process_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > PROCESS_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&process_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = process_test_heap + offset;
			return true;
		}
	}
}

static void init_process_test_environment(void) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)process_test_arena,
         .length = PROCESS_TEST_ARENA_SIZE,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	process_test_heap_offset = 0u;
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(kheap_init(), "kheap_init failed");
}

Test(process, create_initializes_pid_state_and_user_address_space) {
	struct process*            process = NULL;
	struct address_space*      space;
	enum process_create_result result;

	init_process_test_environment();

	result = process_create(&process, "init");
	cr_assert_eq(result, PROCESS_CREATE_OK, "process_create failed: %d", result);
	cr_assert_not_null(process, "process_create did not return a process");
	cr_assert_neq(process->pid, PROCESS_PID_INVALID, "process pid should be valid");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_NEW, "new process should start in NEW state");
	cr_assert_eq(process_thread_count(process), 0u, "new process should not have attached threads");

	space = &process->address_space;
	cr_assert_not_null(space, "process address space should be exposed");
	cr_assert(address_space_is_initialized(space), "process address space should be initialized");
	cr_assert_eq(address_space_total_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);
	cr_assert_eq(address_space_free_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);

	cr_assert(process_destroy(process), "process_destroy failed");
}

Test(process, create_assigns_monotonic_pids) {
	struct process* first  = NULL;
	struct process* second = NULL;
	process_id_t    first_pid;
	process_id_t    second_pid;

	init_process_test_environment();

	cr_assert_eq(process_create(&first, "first"), PROCESS_CREATE_OK);
	cr_assert_eq(process_create(&second, "second"), PROCESS_CREATE_OK);

	first_pid  = first->pid;
	second_pid = second->pid;
	cr_assert_neq(first_pid, PROCESS_PID_INVALID);
	cr_assert_neq(second_pid, PROCESS_PID_INVALID);
	cr_assert(second_pid > first_pid, "second pid should be greater than first pid");

	cr_assert(process_destroy(first), "failed to destroy first process");
	cr_assert(process_destroy(second), "failed to destroy second process");
}

Test(process, create_rejects_missing_output_pointer) {
	init_process_test_environment();

	cr_assert_eq(process_create(NULL, "invalid"), PROCESS_CREATE_INVALID_ARGUMENTS);
}
