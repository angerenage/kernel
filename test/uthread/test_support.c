#include "test_support.h"

#define UTHREAD_TEST_ARENA_SIZE KiB(2048)
#define UTHREAD_TEST_HEAP_SIZE KiB(64)

static uint8_t uthread_test_arena[UTHREAD_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t uthread_test_heap[UTHREAD_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  uthread_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	bytes     = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&uthread_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > UTHREAD_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&uthread_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = uthread_test_heap + offset;
			return true;
		}
	}
}

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE));
	cpu_interrupts_set_ready(cpu_current(), false);
}

void init_uthread_test_environment(void) {
	const struct mem_range memory_map[] = {
		{.base = (uintptr_t)uthread_test_arena, .length = UTHREAD_TEST_ARENA_SIZE, .type = MEM_RANGE_USABLE},
	};

	uthread_test_heap_offset = 0u;
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_userspace_mock_set_context_init_result(true);
	hal_userspace_mock_set_context_init_hook(NULL);
	hal_cpu_local_bind(NULL);
	init_bound_bootstrap_cpu();
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(cpu_current()));
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0));
	cr_assert(vmm_init());
	cr_assert(heap_init());
}

struct process* spawn_owner_process(const char* name) {
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

void terminate_main_thread(struct process* process) {
	if (process != NULL && process->main_thread != NULL) thread_mark_zombie(&process->main_thread->thread);
}
