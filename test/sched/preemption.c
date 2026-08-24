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

Test(sched, remote_tick_is_consumed_only_by_target_cpu) {
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread current;
	struct thread peer;

	sched_regression_init_dual_cpu(&bsp, &ap);
	sched_regression_init_thread(&current, "ap_current", 0x3a0000u, THREAD_PRIORITY_DEFAULT, ap, NULL);
	sched_regression_init_thread(&peer, "ap_peer", 0x3b0000u, THREAD_PRIORITY_DEFAULT, ap, NULL);
	sched_test_set_one_tick_timeslice(&current);
	sched_test_set_one_tick_timeslice(&peer);

	cpu_bind_current(ap);
	cpu_interrupts_set_ready(ap, false);
	sched_set_current(ap, &current);
	cr_assert(sched_make_runnable(&peer), "failed to queue equal-priority AP peer");
	cr_assert(!sched_reschedule_pending(ap), "equal-priority enqueue should wait for timeslice expiry");

	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	hal_cpu_mock_reset_kicks();
	sched_tick_remote(ap);

	cr_assert_eq(current.timeslice_remaining, 1u, "BSP must not mutate the AP current thread's timeslice");
	cr_assert(!sched_reschedule_pending(ap), "remote publication must not run target scheduler logic on the BSP");
	cr_assert_eq(hal_cpu_mock_kick_count(ap), 1u, "remote AP tick should send exactly one kick");

	cpu_bind_current(ap);
	cpu_interrupts_set_ready(ap, false);
	cr_assert(sched_handle_interrupt_exit(), "AP interrupt exit should consume its pending scheduler tick");
	cr_assert_eq(sched_current_thread(), &peer, "AP should rotate to its equal-priority peer after local tick consume");
	cr_assert(thread_is_queued(&current), "AP current thread should be re-queued after local timeslice expiry");

	sched_regression_reset();
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
