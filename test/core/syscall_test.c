#include <core/cpu.h>
#include <core/kheap.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>

#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

#define SYSCALL_TEST_ARENA_SIZE KiB(2048)
#define SYSCALL_TEST_HEAP_SIZE KiB(64)

static uint8_t syscall_test_arena[SYSCALL_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t syscall_test_heap[SYSCALL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  syscall_test_heap_offset;

bool kheap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
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

static void syscall_test_init_scheduler(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
}

static void syscall_test_reset_state(void) {
	hal_clock_stop();
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
}

static void syscall_test_init_process_environment(void) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)syscall_test_arena,
         .length = SYSCALL_TEST_ARENA_SIZE,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	syscall_test_heap_offset = 0u;
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_cpu_local_bind(NULL);

	syscall_test_init_scheduler();
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE), "cpu_set_state failed");
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(kheap_init(), "kheap_init failed");
}

static struct process* syscall_test_spawn_process(const char* name) {
	struct process* process = NULL;
	struct uthread* main_thread;

	cr_assert_eq(process_create(&process, name), PROCESS_OK, "process_create failed");
	cr_assert_not_null(process, "process_create returned NULL process");
	cr_assert_eq(process_spawn_thread(process,
	                                  &main_thread,
	                                  &(const struct process_thread_params){
										  .name       = name,
										  .user_entry = 0x300000u,
										  .detached   = false,
									  }),
	             PROCESS_THREAD_SPAWN_OK,
	             "process_spawn_thread failed");
	return process;
}

static void syscall_test_thread_entry(void* arg) {
	(void)arg;
}

Test(syscall, nop_returns_ok) {
	syscall_result_t result = syscall_dispatch(SYSCALL_NOP, 1u, 2u, 3u, 4u, 5u, 6u);

	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert(syscall_status_is_success(result.status), "nop should return success");
}

Test(syscall, invalid_number_returns_unknown_syscall) {
	syscall_result_t result = syscall_dispatch(UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u, 0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_UNKNOWN_SYSCALL);
	cr_assert_eq(result.value, UINTPTR_MAX);
	cr_assert(syscall_status_is_caller_error(result.status), "unknown syscall should be a caller error");
}

Test(syscall, sleep_ms_fails_without_clock) {
	syscall_result_t result;

	syscall_test_init_scheduler();

	result = syscall_dispatch(SYSCALL_SLEEP_MS, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cr_assert(syscall_status_is_kernel_error(SYSCALL_STATUS_UNAVAILABLE));

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_rejects_unrepresentable_deadline) {
	syscall_result_t result;

	syscall_test_init_scheduler();
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");
	sched_tick();

	result = syscall_dispatch(SYSCALL_SLEEP_MS, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u, "sleep_ms should report arg0 as problematic");

	syscall_test_reset_state();
}

Test(syscall, tick_count_returns_scheduler_ticks) {
	syscall_result_t result;

	syscall_test_init_scheduler();
	sched_tick();
	sched_tick();

	result = syscall_dispatch(SYSCALL_TICK_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 2u);

	syscall_test_reset_state();
}

Test(syscall, getpid_fails_without_current_process) {
	syscall_result_t result;

	syscall_test_init_scheduler();

	result = syscall_dispatch(SYSCALL_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	syscall_test_reset_state();
}

Test(syscall, process_introspection_returns_current_process_values) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/process");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread, "process should have a main thread");

	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, (uintptr_t)process_pid(process));

	result = syscall_dispatch(SYSCALL_GET_PROCESS_THREAD_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);

	syscall_test_reset_state();
}

Test(syscall, yield_dispatches_next_runnable_thread) {
	const struct thread_create_params first_params = {
		.name              = "syscall-yield-first",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "syscall-yield-second",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	syscall_test_init_scheduler();
	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &first, "yield should dispatch first runnable thread");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &second, "yield should rotate to the next runnable thread");
	cr_assert(thread_is_queued(&first), "previous thread should be queued after yielding");

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_zero_yields) {
	const struct thread_create_params first_params = {
		.name              = "syscall-sleep-zero-first",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x320000u,
		.kernel_stack_top  = 0x324000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "syscall-sleep-zero-second",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x330000u,
		.kernel_stack_top  = 0x334000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	syscall_test_init_scheduler();
	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first thread should be running before sleep_ms(0)");
	cr_assert_eq(syscall_dispatch(SYSCALL_SLEEP_MS, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &second, "sleep_ms(0) should yield to the next runnable thread");

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_blocks_until_deadline) {
	const struct thread_create_params sleeper_params = {
		.name              = "syscall-sleeper",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x340000u,
		.kernel_stack_top  = 0x344000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params worker_params = {
		.name              = "syscall-worker",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x350000u,
		.kernel_stack_top  = 0x354000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread sleeper;
	struct thread worker;

	syscall_test_init_scheduler();
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");
	cr_assert(thread_init(&sleeper, &sleeper_params), "thread_init failed for sleeper thread");
	cr_assert(thread_init(&worker, &worker_params), "thread_init failed for worker thread");
	worker.timeslice_ticks     = 1u;
	worker.timeslice_remaining = 1u;
	cr_assert(sched_make_runnable(&sleeper), "failed to make sleeper runnable");
	cr_assert(sched_make_runnable(&worker), "failed to make worker runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should dispatch first");
	cr_assert_eq(syscall_dispatch(SYSCALL_SLEEP_MS, 2u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sleeper.state, THREAD_STATE_BLOCKED, "sleeper should block while sleeping");
	cr_assert_eq(sleeper.block_reason, THREAD_BLOCK_SLEEP, "sleeper block reason should be sleep");
	cr_assert_eq(sched_current_thread(), &worker, "worker should run after sleeper blocks");

	sched_tick();
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "sleeper should not wake before deadline");
	sched_tick();
	cr_assert_eq(sleeper.state, THREAD_STATE_READY, "sleeper should be ready after deadline");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "sleeper should be queued after wake");
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should preempt worker once sleeper wakes");
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should run after being woken");

	syscall_test_reset_state();
}
