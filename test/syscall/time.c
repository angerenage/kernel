#include "test_support.h"

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
