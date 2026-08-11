#include "test_support.h"

#define SYSCALL_TEST_ARENA_SIZE KiB(2048)
#define SYSCALL_TEST_HEAP_SIZE KiB(256)

static uint8_t syscall_test_arena[SYSCALL_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t syscall_test_heap[SYSCALL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  syscall_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	bytes     = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&syscall_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > SYSCALL_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&syscall_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = syscall_test_heap + offset;
			return true;
		}
	}
}

void syscall_test_init_scheduler(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(cpu_current()));
}

void syscall_test_reset_state(void) {
	hal_clock_stop();
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
}

void syscall_test_init_process_environment(void) {
	const struct mem_range memory_map[] = {
		{.base = (uintptr_t)syscall_test_arena, .length = SYSCALL_TEST_ARENA_SIZE, .type = MEM_RANGE_USABLE},
	};

	syscall_test_heap_offset = 0u;
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_cpu_local_bind(NULL);
	syscall_test_init_scheduler();
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE));
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0));
	cr_assert(vmm_init());
	cr_assert(heap_init());
}

struct process* syscall_test_spawn_process(const char* name) {
	struct process* process = NULL;
	struct uthread* main_thread;

	cr_assert_eq(process_create(&process, name), PROCESS_OK);
	cr_assert_not_null(process);
	cr_assert_eq(process_spawn_thread(
					 process,
					 &main_thread,
					 &(const struct process_thread_params){.name = name, .user_entry = 0x300000u, .detached = false}),
	             PROCESS_THREAD_SPAWN_OK);
	return process;
}

void syscall_test_thread_entry(void* arg) {
	(void)arg;
}

syscall_result_t syscall_test_cap_handler(const struct cap_request* request) {
	struct syscall_test_cap_request  input;
	struct syscall_test_cap_response output;

	if (request == NULL || request->request == NULL || request->request_size < sizeof(input))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (request->response == NULL || request->response_capacity < sizeof(output))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&input, request->request, sizeof(input));
	output.value = input.value + 1u;
	memcpy(request->response, &output, sizeof(output));
	return syscall_result_ok(sizeof(output));
}
