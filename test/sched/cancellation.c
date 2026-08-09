#include "test_support.h"

Test(sched, pending_cancellation_aborts_wait_queue_handoff) {
	const struct thread_create_params params = {
		.name              = "cancelled_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3a0000u,
		.kernel_stack_top  = 0x3a4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            current;
	struct thread_wait_queue wait_queue;
	struct irq_state         wait_state;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&current, &params), "thread_init failed for cancelled waiter");
	sched_set_current(cpu_current(), &current);

	cr_assert(thread_request_cancel(&current), "cancellation request should succeed");
	wait_state = spinlock_lock_irqsave(&wait_queue.lock);
	cr_assert_not(sched_block_current_locked(&wait_queue, THREAD_BLOCK_SIGNAL, wait_state),
	              "a pending cancellation must abort the wait handoff");
	cr_assert_eq(sched_current_thread(), &current, "cancelled waiter must remain current");
	cr_assert_eq(current.state, THREAD_STATE_RUNNING, "cancelled waiter must remain running");
	cr_assert_null(current.blocked_queue, "cancelled waiter must not retain a blocked queue");
	cr_assert_eq(current.wait_status, THREAD_WAIT_STATUS_NONE, "cancelled waiter status must be cleared");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "cancelled waiter must not enter the wait queue");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "cancelled current thread must not be re-queued");

	reset_test_state();
}

Test(sched, pending_cancellation_aborts_timed_wait_handoff) {
	const struct thread_create_params params = {
		.name              = "cancelled_timed_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3b0000u,
		.kernel_stack_top  = 0x3b4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            current;
	struct thread_wait_queue wait_queue;
	struct irq_state         wait_state;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&current, &params), "thread_init failed for cancelled timed waiter");
	sched_set_current(cpu_current(), &current);

	cr_assert(thread_request_cancel(&current), "cancellation request should succeed");
	wait_state = spinlock_lock_irqsave(&wait_queue.lock);
	cr_assert_not(
		sched_block_current_until_locked(&wait_queue, THREAD_BLOCK_JOIN, sched_tick_count() + 10u, wait_state),
		"a pending cancellation must abort the timed wait handoff");
	cr_assert_eq(sched_current_thread(), &current, "cancelled timed waiter must remain current");
	cr_assert_eq(current.state, THREAD_STATE_RUNNING, "cancelled timed waiter must remain running");
	cr_assert_null(current.blocked_queue, "cancelled timed waiter must not retain a blocked queue");
	cr_assert_null(current.sleep_queue_next, "cancelled timed waiter must not enter the sleep queue");
	cr_assert_eq(current.wake_deadline_tick, 0u, "cancelled timed waiter must clear its deadline");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "cancelled timed waiter must not enter the wait queue");

	reset_test_state();
}

Test(sched, cancellation_during_block_handoff_requeues_current_thread) {
	const struct thread_create_params params = {
		.name              = "handoff_cancelled",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3f0000u,
		.kernel_stack_top  = 0x3f4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            current;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&current, &params), "thread_init failed for handoff cancellation");
	sched_set_current(cpu_current(), &current);

	thread_mark_blocked(&current, THREAD_BLOCK_SIGNAL);
	current.blocked_queue = &wait_queue;
	current.wait_status   = THREAD_WAIT_STATUS_PENDING;
	wait_queue.head       = &current;
	wait_queue.tail       = &current;
	wait_queue.depth      = 1u;

	cr_assert(thread_request_cancel(&current), "cancellation during block handoff must succeed");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "the cancelled handoff must leave the wait queue");
	cr_assert_eq(current.state, THREAD_STATE_READY, "the cancelled handoff must become ready");
	cr_assert(thread_is_queued(&current), "the cancelled handoff must remain runnable");
	cr_assert_eq(current.wait_status, THREAD_WAIT_STATUS_CANCELED, "the cancellation result must remain observable");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "the cancelled handoff must be queued to run");

	reset_test_state();
}

Test(sched, repeated_cancellation_of_blocked_waiter_never_duplicates_runnable_state) {
	struct thread            waiter;
	struct thread_wait_queue queue;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&waiter, "waiter", 0x318000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	thread_wait_queue_init(&queue);

	thread_mark_blocked(&waiter, THREAD_BLOCK_WAIT_QUEUE);
	waiter.blocked_queue = &queue;
	waiter.wait_status   = THREAD_WAIT_STATUS_PENDING;
	queue.head           = &waiter;
	queue.tail           = &waiter;
	queue.depth          = 1u;

	cr_assert(thread_request_cancel(&waiter), "cancel request failed");
	cr_assert_eq(thread_wait_queue_depth(&queue), 0u, "cancel must detach waiter");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "cancel must queue waiter exactly once");
	cr_assert(thread_is_queued(&waiter), "cancelled waiter must be runnable");

	sched_cancel_thread(&waiter);
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "repeated cancellation must not duplicate runnable state");

	sched_regression_reset();
}

Test(sched, pending_cancellation_cannot_be_lost_when_current_enters_sleep) {
	struct thread sleeper;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&sleeper, "sleeper", 0x338000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_set_current(cpu_current(), &sleeper);

	cr_assert(thread_request_cancel(&sleeper), "cancel request failed");
	cr_assert(thread_should_cancel(&sleeper), "cancellation must be actionable");
	(void)sched_sleep_until_tick(sched_tick_count() + 8u);

	cr_assert_neq(sleeper.state,
	              THREAD_STATE_BLOCKED,
	              "a cancellation that was already pending must not become stranded behind a new sleep");
	cr_assert_eq(sleeper.wake_deadline_tick, 0u, "cancelled sleeper must not retain a sleep deadline");
	cr_assert(sched_current_thread() == &sleeper || thread_is_queued(&sleeper),
	          "cancelled sleeper must remain able to reach its cancellation point");

	sched_regression_reset();
}
