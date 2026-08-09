#include "test_support.h"

Test(kthread, join_terminated_thread_returns_exit_code_and_detaches) {
	const struct thread_create_params params = {
		.name              = "target",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread     target    = {.stack_id = VMM_ID_INVALID};
	thread_exit_code_t exit_code = 0u;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&target.thread, &params), "thread_init failed");

	thread_mark_exiting(&target.thread, 123u);
	cr_assert(!kthread_timed_join(&target, 0u, &exit_code),
	          "EXITING thread must not be joinable before its stack is reap-safe");
	cr_assert_eq(exit_code, 0u, "failed join must not publish an exit code");
	thread_mark_zombie(&target.thread);
	cr_assert(kthread_join(&target, &exit_code), "kthread_join should succeed for terminated joinable thread");
	cr_assert_eq(exit_code, 123u, "kthread_join should publish target exit code");

	exit_code = 0u;
	cr_assert(kthread_timed_join(&target, 0u, &exit_code),
	          "kthread_timed_join should succeed immediately for terminated joinable thread");
	cr_assert_eq(exit_code, 123u, "kthread_timed_join should publish target exit code");

	reset_test_state();
}

Test(kthread, timed_join_times_out_without_consuming_joinability) {
	const struct thread_create_params target_params = {
		.name              = "target",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x318000u,
		.kernel_stack_top  = 0x31c000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params joiner_params = {
		.name              = "joiner",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x31c000u,
		.kernel_stack_top  = 0x320000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner_params = {
		.name              = "runner",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x324000u,
		.kernel_stack_top  = 0x328000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread     target = {.stack_id = VMM_ID_INVALID};
	struct kthread     joiner = {.stack_id = VMM_ID_INVALID};
	struct thread      runner;
	thread_exit_code_t exit_code = 0xdeadbeefu;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");
	cr_assert(thread_init(&target.thread, &target_params), "target thread_init failed");
	cr_assert(thread_init(&joiner.thread, &joiner_params), "joiner thread_init failed");
	cr_assert(thread_init(&runner, &runner_params), "runner thread_init failed");
	thread_mark_blocked(&target.thread, THREAD_BLOCK_PARKED);
	cr_assert(sched_make_runnable(&runner), "runner should become runnable during timed join timeout");

	hal_cpu_mock_set_context_switch_hook(kthread_test_timeout_context_switch_hook);
	sched_set_current(cpu_current(), &joiner.thread);
	cr_assert(!kthread_timed_join(&target, 2u, &exit_code), "kthread_timed_join should time out");
	cr_assert_eq(kthread_test_timeout_hook_runs, 1u, "timeout hook should run exactly once");
	cr_assert_eq(exit_code, 0xdeadbeefu, "timed out join must not publish an exit code");
	cr_assert_eq(thread_wait_queue_depth(&target.thread.join_wait_queue), 0u, "timed out joiner should be dequeued");
	cr_assert(thread_is_joinable(&target.thread), "timed out join must leave the target joinable");
	cr_assert_eq(sched_current_thread(), &joiner.thread, "joiner should resume after timeout");

	hal_clock_stop();
	reset_test_state();
}
