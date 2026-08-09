#include "test_support.h"

Test(thread, lifecycle_helpers_update_state_flags_and_links) {
	const struct thread_create_params params = {
		.name              = "lifecycle",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x600000u,
		.kernel_stack_top  = 0x604000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread thread;

	init_bound_bootstrap_cpu();
	cr_assert(thread_init(&thread, &params), "thread_init failed");

	thread.flags |= THREAD_FLAG_QUEUED;
	thread.run_queue_next  = &thread;
	thread.wait_queue_next = &thread;
	thread_mark_ready(&thread, cpu_current());
	cr_assert_eq(thread.state, THREAD_STATE_READY, "thread_mark_ready should move thread to READY");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_ready should clear block reason");
	cr_assert_eq(thread.cpu, cpu_current(), "thread_mark_ready should bind target cpu");
	cr_assert(!thread_is_queued(&thread), "thread_mark_ready should clear queued flag");
	cr_assert_null(thread.run_queue_next, "thread_mark_ready should clear run queue linkage");
	cr_assert_null(thread.wait_queue_next, "thread_mark_ready should clear wait queue linkage");

	thread.flags |= THREAD_FLAG_QUEUED;
	thread.run_queue_next  = &thread;
	thread.wait_queue_next = &thread;
	thread_mark_blocked(&thread, THREAD_BLOCK_JOIN);
	cr_assert_eq(thread.state, THREAD_STATE_BLOCKED, "thread_mark_blocked should move thread to BLOCKED");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_JOIN, "thread_mark_blocked should record block reason");
	cr_assert(!thread_is_queued(&thread), "thread_mark_blocked should clear queued flag");
	cr_assert_null(thread.run_queue_next, "thread_mark_blocked should clear run queue linkage");
	cr_assert_null(thread.wait_queue_next, "thread_mark_blocked should clear wait queue linkage");

	thread_mark_running(&thread, cpu_current());
	cr_assert_eq(thread.state, THREAD_STATE_RUNNING, "thread_mark_running should move thread to RUNNING");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_running should clear block reason");

	cr_assert(thread_request_cancel(&thread), "thread_request_cancel should succeed on live thread");
	cr_assert(thread_cancel_requested(&thread), "cancel request flag should be visible");
	cr_assert(thread_cancel_enabled(&thread), "fresh thread should start with cancellation enabled");
	cr_assert(thread_should_cancel(&thread), "pending cancellation should be actionable while enabled");
	thread_set_cancel_enabled(&thread, false);
	cr_assert((thread.flags & THREAD_FLAG_CANCEL_DISABLED) != 0u, "cancel disable flag should be set");
	cr_assert(!thread_cancel_enabled(&thread), "cancel disable helper should reflect the masked state");
	cr_assert(!thread_should_cancel(&thread), "masked cancellation should not trip a cancellation point");
	thread_set_cancel_enabled(&thread, true);
	cr_assert((thread.flags & THREAD_FLAG_CANCEL_DISABLED) == 0u, "cancel disable flag should clear");
	cr_assert(thread_cancel_enabled(&thread), "cancel enable helper should reflect the unmasked state");
	cr_assert(thread_should_cancel(&thread), "re-enabling cancellation should re-arm pending cancellation");

	thread_mark_exiting(&thread, 99u);
	cr_assert_eq(thread.state, THREAD_STATE_EXITING, "thread_mark_exiting should move thread to EXITING");
	cr_assert_eq(thread.exit_code, 99u, "thread_mark_exiting should publish exit code");
	cr_assert(thread_is_terminated(&thread), "EXITING thread should count as terminated");
	cr_assert(!thread_is_reap_safe(&thread), "EXITING thread must not be reclaimable");
	cr_assert(!thread_request_cancel(&thread), "terminated thread should reject new cancel requests");

	thread_mark_zombie(&thread);
	cr_assert_eq(thread.state, THREAD_STATE_ZOMBIE, "thread_mark_zombie should move thread to ZOMBIE");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_zombie should clear block reason");
	cr_assert(thread_is_reap_safe(&thread), "ZOMBIE thread should be reclaimable");

	reset_test_state();
}

Test(thread, detach_and_idle_helpers_follow_joinability_rules) {
	const struct thread_create_params detached_params = {
		.name              = "detached",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x700000u,
		.kernel_stack_top  = 0x704000u,
		.preferred_cpu     = NULL,
		.detached          = true,
	};
	const struct thread_create_params joinable_params = {
		.name              = "joinable",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x710000u,
		.kernel_stack_top  = 0x714000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread detached_thread;
	struct thread joinable_thread;
	struct thread idle_thread;

	init_bound_bootstrap_cpu();

	cr_assert(thread_init(&detached_thread, &detached_params), "detached thread_init failed");
	cr_assert(!thread_is_joinable(&detached_thread), "thread created detached should not be joinable");

	cr_assert(thread_init(&joinable_thread, &joinable_params), "joinable thread_init failed");
	cr_assert(thread_is_joinable(&joinable_thread), "fresh thread should be joinable");
	cr_assert(thread_detach(&joinable_thread), "thread_detach should succeed once");
	cr_assert(!thread_is_joinable(&joinable_thread), "detached thread should stop being joinable");
	cr_assert(!thread_detach(&joinable_thread), "thread_detach should fail once already detached");

	thread_init_idle(&idle_thread, cpu_current(), "idle/test");
	cr_assert(thread_is_idle(&idle_thread), "idle thread should carry idle flag");
	cr_assert(!thread_is_joinable(&idle_thread), "idle thread must never be joinable");
	cr_assert(!thread_request_cancel(&idle_thread), "idle thread must reject cancellation");
	cr_assert(!thread_detach(&idle_thread), "idle thread must reject detach");
	cr_assert_eq(idle_thread.base_priority, THREAD_PRIORITY_MIN, "idle threads should carry minimum priority");
	cr_assert_eq(idle_thread.effective_priority, THREAD_PRIORITY_MIN, "idle threads should carry minimum priority");
	cr_assert_eq(idle_thread.timeslice_ticks, 0u, "idle threads should not consume scheduler timeslices");
	cr_assert_eq(idle_thread.timeslice_remaining, 0u, "idle threads should not carry a timeslice budget");
	cr_assert_eq(
		thread_wait_queue_depth(&idle_thread.join_wait_queue), 0u, "idle thread wait queue should start empty");
	cr_assert_eq(
		thread_wait_queue_depth(&idle_thread.park_wait_queue), 0u, "idle thread park wait queue should start empty");

	reset_test_state();
}

Test(thread, lifecycle_transitions_preserve_persistent_flags_and_clear_scheduler_links) {
	struct thread  thread;
	const uint32_t persistent_flags =
		THREAD_FLAG_DETACHED | THREAD_FLAG_CANCEL_PENDING | THREAD_FLAG_PARK_PERMIT | THREAD_FLAG_INTERRUPT_PENDING;

	irq_enable_local();
	thread_regression_init(&thread, "lifecycle", 0x830000u, THREAD_PRIORITY_DEFAULT);

	thread.flags |= persistent_flags | THREAD_FLAG_QUEUED | THREAD_FLAG_WAIT_INTERRUPTIBLE;
	thread.run_queue_next     = &thread;
	thread.wait_queue_next    = &thread;
	thread.sleep_queue_next   = &thread;
	thread.wake_deadline_tick = 123u;

	thread_mark_ready(&thread, NULL);
	cr_assert_eq(thread.flags & persistent_flags, persistent_flags, "READY must preserve persistent flags");
	cr_assert_eq(thread.flags & (THREAD_FLAG_QUEUED | THREAD_FLAG_WAIT_INTERRUPTIBLE),
	             0u,
	             "READY must clear transient scheduler flags");
	cr_assert_null(thread.run_queue_next, "READY must clear run linkage");
	cr_assert_null(thread.wait_queue_next, "READY must clear wait linkage");
	cr_assert_null(thread.sleep_queue_next, "READY must clear sleep linkage");
	cr_assert_eq(thread.wake_deadline_tick, 0u, "READY must clear sleep deadline");

	thread.flags |= THREAD_FLAG_QUEUED | THREAD_FLAG_WAIT_INTERRUPTIBLE;
	thread.run_queue_next     = &thread;
	thread.wait_queue_next    = &thread;
	thread.sleep_queue_next   = &thread;
	thread.wake_deadline_tick = 456u;
	thread_mark_blocked(&thread, THREAD_BLOCK_JOIN);
	cr_assert_eq(thread.flags & persistent_flags, persistent_flags, "BLOCKED must preserve persistent flags");
	cr_assert_eq(thread.flags & (THREAD_FLAG_QUEUED | THREAD_FLAG_WAIT_INTERRUPTIBLE),
	             0u,
	             "BLOCKED must clear transient scheduler flags");
	cr_assert_null(thread.run_queue_next, "BLOCKED must clear run linkage");
	cr_assert_null(thread.wait_queue_next, "BLOCKED must clear wait linkage");
	cr_assert_null(thread.sleep_queue_next, "BLOCKED must clear sleep linkage");
	cr_assert_eq(thread.wake_deadline_tick, 0u, "BLOCKED must clear stale deadline");

	thread_mark_running(&thread, NULL);
	cr_assert_eq(thread.flags & persistent_flags, persistent_flags, "RUNNING must preserve persistent flags");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "RUNNING must clear block reason");

	thread_regression_reset();
}
