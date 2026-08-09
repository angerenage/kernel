#include "test_support.h"

Test(kthread, canceled_running_thread_exits_with_cancel_code_at_cancellation_point) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x330000u,
		.kernel_stack_top  = 0x334000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	sched_set_current(cpu_current(), &worker.thread);
	cr_assert(kthread_cancel(&worker), "kthread_cancel should mark the current worker for cancellation");

	hal_cpu_mock_set_context_switch_hook(kthread_test_cancel_context_switch_hook);
	kthread_test_cancel_hook_armed = true;
	if (setjmp(kthread_test_cancel_jmp) == 0) {
		kthread_yield();
		cr_assert_fail("kthread_yield should not return once cancellation exits the running thread");
	}

	sched_complete_context_switch();
	cr_assert_eq(worker.thread.state, THREAD_STATE_ZOMBIE, "context-switch completion should publish ZOMBIE");
	cr_assert_eq(worker.thread.exit_code,
	             THREAD_EXIT_CODE_CANCELLED,
	             "canceled worker should publish the dedicated cancellation exit code");
	cr_assert_eq(sched_current_thread(),
	             sched_idle_thread(cpu_current()),
	             "scheduler should switch away from the canceled worker");

	reset_test_state();
}

Test(kthread, cancel_wakes_sleeping_thread_so_it_can_reach_a_cancellation_point) {
	const struct thread_create_params params = {
		.name              = "sleeper",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x340000u,
		.kernel_stack_top  = 0x344000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread sleeper = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&sleeper.thread, &params), "thread_init failed");

	sched_set_current(cpu_current(), &sleeper.thread);
	kthread_test_sleep_cancel_target     = &sleeper;
	kthread_test_sleep_cancel_hook_armed = true;
	hal_cpu_mock_set_context_switch_hook(kthread_test_sleep_cancel_context_switch_hook);
	cr_assert(sched_sleep_until_tick(sched_tick_count() + 8u), "sched_sleep_until_tick failed");

	cr_assert(thread_cancel_requested(&sleeper.thread), "cancel flag should stay visible after wakeup");
	cr_assert_eq(sleeper.thread.state, THREAD_STATE_READY, "cancel should move the sleeping thread back to READY");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "canceled sleeper should be queued so it can run and exit");

	reset_test_state();
}
