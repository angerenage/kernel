#include "test_support.h"

#include <base/vmm.h>

#define PROCESS_TEST_ARENA_SIZE KiB(2048)
#define PROCESS_TEST_HEAP_SIZE KiB(256)

static uint8_t process_test_arena[PROCESS_TEST_ARENA_SIZE] __attribute__((aligned(VMM_PAGE_SIZE)));
static uint8_t process_test_heap[PROCESS_TEST_HEAP_SIZE] __attribute__((aligned(VMM_PAGE_SIZE)));
static size_t  process_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	bytes     = page_count * VMM_PAGE_SIZE;
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

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE));
	cpu_interrupts_set_ready(cpu_current(), false);
}

void init_process_test_environment(void) {
	const struct mem_range memory_map[] = {
		{.base = (uintptr_t)process_test_arena, .length = PROCESS_TEST_ARENA_SIZE, .type = MEM_RANGE_USABLE},
	};

	process_test_heap_offset = 0u;
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
	cr_assert(vm_init());
	cr_assert(heap_init());
}

static enum process_result process_test_result_from_thread_spawn(enum process_thread_spawn_result result) {
	switch (result) {
	case PROCESS_THREAD_SPAWN_OK:
		return PROCESS_OK;
	case PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS:
		return PROCESS_INVALID_ARGUMENTS;
	case PROCESS_THREAD_SPAWN_NO_MEMORY:
		return PROCESS_NO_MEMORY;
	case PROCESS_THREAD_SPAWN_STACK_ALLOC_FAILED:
		return PROCESS_THREAD_STACK_ALLOC_FAILED;
	case PROCESS_THREAD_SPAWN_CONTEXT_UNSUPPORTED:
		return PROCESS_THREAD_CONTEXT_UNSUPPORTED;
	case PROCESS_THREAD_SPAWN_SCHEDULER_REJECTED:
		return PROCESS_THREAD_SCHEDULER_REJECTED;
	case PROCESS_THREAD_SPAWN_REAPER_UNAVAILABLE:
		return PROCESS_THREAD_REAPER_UNAVAILABLE;
	case PROCESS_THREAD_SPAWN_ID_EXHAUSTED:
	default:
		return PROCESS_THREAD_ID_EXHAUSTED;
	}
}

enum process_result create_process_with_main_thread(struct process**                   out_process,
                                                    const struct process_spawn_params* params) {
	struct process*                  process = NULL;
	struct uthread*                  main_thread;
	enum process_result              process_result;
	enum process_thread_spawn_result thread_result;

	if (out_process == NULL || params == NULL) return PROCESS_INVALID_ARGUMENTS;
	*out_process   = NULL;
	process_result = process_create(&process, params->name);
	if (process_result != PROCESS_OK) return process_result;
	thread_result =
		process_spawn_thread(process,
	                         &main_thread,
	                         &(const struct process_thread_params){.name             = params->name,
	                                                               .user_entry       = params->user_entry,
	                                                               .arg_data         = params->arg_data,
	                                                               .arg_size         = params->arg_size,
	                                                               .user_stack_pages = params->user_stack_pages,
	                                                               .preferred_cpu    = params->preferred_cpu,
	                                                               .detached         = false});
	if (thread_result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_destroy(process);
		return process_test_result_from_thread_spawn(thread_result);
	}
	*out_process = process;
	return PROCESS_OK;
}

void terminate_main_thread(struct process* process) {
	if (process != NULL && process->main_thread != NULL) thread_mark_zombie(&process->main_thread->thread);
}

void terminate_process_thread(struct uthread* thread) {
	if (thread != NULL) thread_mark_zombie(&thread->thread);
}
