#include "test_support.h"

Test(sched, high_priority_thread_preempts_lower_priority_current_at_interrupt_exit) {
	const struct thread_create_params low_params = {
		.name              = "low_current",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30a000u,
		.kernel_stack_top  = 0x30e000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT - 2,
		.detached          = false,
	};
	const struct thread_create_params high_params = {
		.name              = "high_ready",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30e000u,
		.kernel_stack_top  = 0x312000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 2,
		.detached          = false,
	};
	struct thread low;
	struct thread high;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&low, &low_params), "thread_init failed for low-priority current thread");
	cr_assert(thread_init(&high, &high_params), "thread_init failed for high-priority runnable thread");

	sched_set_current(cpu_current(), &low);
	cr_assert(sched_make_runnable(&high), "failed to make high-priority thread runnable");
	cr_assert(sched_reschedule_pending(cpu_current()), "higher-priority enqueue should request deferred preemption");
	cr_assert_eq(sched_current_thread(), &low, "preemption should wait for interrupt-exit handling");

	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume the priority preemption request");
	cr_assert_eq(sched_current_thread(), &high, "high-priority thread should preempt lower-priority current thread");
	cr_assert(thread_is_queued(&low), "preempted lower-priority thread should be re-queued");

	reset_test_state();
}

Test(sched, timeslice_expiry_rotates_equal_priority_threads_only) {
	const struct thread_create_params high_params = {
		.name              = "high_current",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x312000u,
		.kernel_stack_top  = 0x316000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 3,
		.detached          = false,
	};
	const struct thread_create_params low_params = {
		.name              = "low_queued",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x316000u,
		.kernel_stack_top  = 0x31a000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT - 3,
		.detached          = false,
	};
	const struct thread_create_params peer_params = {
		.name              = "high_peer",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x31a000u,
		.kernel_stack_top  = 0x31e000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 3,
		.detached          = false,
	};
	struct thread high;
	struct thread low;
	struct thread peer;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&high, &high_params), "thread_init failed for high-priority current thread");
	cr_assert(thread_init(&low, &low_params), "thread_init failed for low-priority queued thread");
	cr_assert(thread_init(&peer, &peer_params), "thread_init failed for high-priority peer thread");
	sched_test_set_one_tick_timeslice(&high);
	sched_test_set_one_tick_timeslice(&low);
	sched_test_set_one_tick_timeslice(&peer);

	sched_set_current(cpu_current(), &high);
	cr_assert(sched_make_runnable(&low), "failed to make low-priority thread runnable");
	sched_tick();
	cr_assert(!sched_reschedule_pending(cpu_current()),
	          "lower-priority queued work should not consume the current priority band's timeslice");
	cr_assert(!sched_handle_interrupt_exit(), "no preemption should be pending for lower-priority queued work");
	cr_assert_eq(sched_current_thread(), &high, "high-priority current thread should keep running");

	cr_assert(sched_make_runnable(&peer), "failed to make equal-priority peer runnable");
	sched_tick();
	cr_assert(sched_reschedule_pending(cpu_current()),
	          "equal-priority runnable work should rotate on timeslice expiry");
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume equal-priority timeslice rotation");
	cr_assert_eq(sched_current_thread(), &peer, "equal-priority peer should run after current timeslice expires");
	cr_assert(thread_is_queued(&high), "preempted equal-priority thread should be queued for round-robin");
	cr_assert(thread_is_queued(&low), "lower-priority thread should remain queued behind the priority band");

	reset_test_state();
}

Test(sched, timer_tick_requests_and_consumes_timeslice_preemption) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x315000u,
		.kernel_stack_top  = 0x319000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x319000u,
		.kernel_stack_top  = 0x31d000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread      first;
	struct thread      second;
	struct sched_stats stats_before;
	struct sched_stats stats_after;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	sched_test_set_one_tick_timeslice(&first);
	sched_test_set_one_tick_timeslice(&second);

	sched_set_current(cpu_current(), &first);
	cr_assert(sched_make_runnable(&second), "failed to make second runnable");
	sched_get_stats(&stats_before);

	sched_tick();
	cr_assert(sched_reschedule_pending(cpu_current()), "timeslice expiry should request a deferred reschedule");
	cr_assert_eq(sched_current_thread(), &first, "preemption should wait for interrupt-exit handling");

	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume the pending reschedule");
	cr_assert_eq(sched_current_thread(), &second, "interrupt exit should dispatch the next runnable thread");
	cr_assert(thread_is_queued(&first), "preempted thread should be re-queued");

	sched_get_stats(&stats_after);
	cr_assert_eq(stats_after.timeslice_preempt_count,
	             stats_before.timeslice_preempt_count + 1u,
	             "timeslice preemption counter should increment");
	cr_assert_eq(stats_after.context_switch_count,
	             stats_before.context_switch_count + 1u,
	             "context switch counter should increment");

	reset_test_state();
}

Test(sched, timer_preemption_rotates_non_yielding_workers) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x380000u,
		.kernel_stack_top  = 0x384000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x390000u,
		.kernel_stack_top  = 0x394000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;
	size_t        first_progress  = 0u;
	size_t        second_progress = 0u;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first worker");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second worker");
	sched_test_set_one_tick_timeslice(&first);
	sched_test_set_one_tick_timeslice(&second);
	cr_assert(sched_make_runnable(&first), "failed to make first worker runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second worker runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first worker should dispatch first");

	for (size_t i = 0; i < 6u; i++) {
		if (sched_current_thread() == &first) first_progress++;
		if (sched_current_thread() == &second) second_progress++;

		sched_tick();
		(void)sched_handle_interrupt_exit();
	}

	cr_assert_gt(first_progress, 0u, "first worker should make progress without yielding explicitly");
	cr_assert_gt(second_progress, 0u, "second worker should make progress without yielding explicitly");

	reset_test_state();
}
