#include "test_support.h"

Test(sched, block_and_wake_preserve_wait_queue_fifo_order) {
	const struct thread_create_params first_params = {
		.name              = "first_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x320000u,
		.kernel_stack_top  = 0x324000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x330000u,
		.kernel_stack_top  = 0x334000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            first;
	struct thread            second;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first waiter");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second waiter");
	cr_assert(sched_make_runnable(&first), "failed to make first waiter runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second waiter runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first waiter should run first");

	sched_block_current(&wait_queue, THREAD_BLOCK_JOIN);
	cr_assert_eq(first.state, THREAD_STATE_BLOCKED, "first waiter should be blocked");
	cr_assert_eq(first.block_reason, THREAD_BLOCK_JOIN, "first waiter block reason mismatch");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 1u, "wait queue should contain first waiter");
	cr_assert_eq(sched_current_thread(), &second, "second waiter should be dispatched next");

	sched_block_current(&wait_queue, THREAD_BLOCK_SLEEP);
	cr_assert_eq(second.state, THREAD_STATE_BLOCKED, "second waiter should be blocked");
	cr_assert_eq(second.block_reason, THREAD_BLOCK_SLEEP, "second waiter block reason mismatch");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 2u, "wait queue should contain both waiters");
	cr_assert_eq(
		sched_current_thread(), sched_idle_thread(cpu_current()), "idle thread should run with no runnable work");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "run queue should be empty while both waiters sleep");

	cr_assert_eq(sched_wake_all(&wait_queue), 2u, "sched_wake_all should wake both waiters");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "wait queue should be empty after wake_all");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "both waiters should be runnable after wake_all");
	cr_assert_eq(first.state, THREAD_STATE_READY, "first waiter should be READY after wake");
	cr_assert_eq(second.state, THREAD_STATE_READY, "second waiter should be READY after wake");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "wake_all should preserve FIFO order for first waiter");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &second, "yield should rotate to the second waiter");

	reset_test_state();
}

Test(sched, wake_one_skips_stale_timed_out_waiters) {
	const struct thread_create_params stale_params = {
		.name              = "stale_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x360000u,
		.kernel_stack_top  = 0x364000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params live_params = {
		.name              = "live_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x370000u,
		.kernel_stack_top  = 0x374000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            stale;
	struct thread            live;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);

	cr_assert(thread_init(&stale, &stale_params), "thread_init failed for stale waiter");
	cr_assert(thread_init(&live, &live_params), "thread_init failed for live waiter");

	thread_mark_blocked(&stale, THREAD_BLOCK_MUTEX);
	stale.blocked_queue   = &wait_queue;
	stale.wait_status     = THREAD_WAIT_STATUS_TIMED_OUT;
	stale.wait_queue_next = &live;

	thread_mark_blocked(&live, THREAD_BLOCK_JOIN);
	live.wait_queue_next = NULL;

	wait_queue.head  = &stale;
	wait_queue.tail  = &live;
	wait_queue.depth = 2u;

	cr_assert(sched_wake_one(&wait_queue),
	          "sched_wake_one should skip the stale timed waiter and wake the live waiter");
	cr_assert_eq(wait_queue.depth, 0u, "wait queue should be empty after consuming stale and live entries");
	cr_assert_null(wait_queue.head, "wait queue head should clear after wake");
	cr_assert_null(wait_queue.tail, "wait queue tail should clear after wake");
	cr_assert_eq(live.state, THREAD_STATE_READY, "live waiter should become ready");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "live waiter should be queued to run");

	reset_test_state();
}

Test(sched, wake_during_block_handoff_requeues_current_thread) {
	const struct thread_create_params params = {
		.name              = "handoff_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3e0000u,
		.kernel_stack_top  = 0x3e4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            current;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&current, &params), "thread_init failed for handoff waiter");
	sched_set_current(cpu_current(), &current);

	thread_mark_blocked(&current, THREAD_BLOCK_SIGNAL);
	current.blocked_queue = &wait_queue;
	current.wait_status   = THREAD_WAIT_STATUS_PENDING;
	wait_queue.head       = &current;
	wait_queue.tail       = &current;
	wait_queue.depth      = 1u;

	cr_assert(sched_wake_one(&wait_queue), "a wake during block handoff must remain deliverable");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "the handoff waiter must leave the wait queue");
	cr_assert_eq(current.state, THREAD_STATE_READY, "the handoff waiter must become ready");
	cr_assert(thread_is_queued(&current), "the handoff waiter must be queued even while still current");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "the handoff waiter must remain runnable");

	reset_test_state();
}

Test(sched, wake_all_skips_terminated_waiters) {
	const struct thread_create_params stale_params = {
		.name              = "terminated_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3c0000u,
		.kernel_stack_top  = 0x3c4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params live_params = {
		.name              = "live_after_terminated",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3d0000u,
		.kernel_stack_top  = 0x3d4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            stale;
	struct thread            live;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&stale, &stale_params), "thread_init failed for terminated waiter");
	cr_assert(thread_init(&live, &live_params), "thread_init failed for live waiter");

	thread_mark_exiting(&stale, 1u);
	stale.wait_queue_next = &live;
	thread_mark_blocked(&live, THREAD_BLOCK_JOIN);
	live.wait_queue_next = NULL;
	wait_queue.head      = &stale;
	wait_queue.tail      = &live;
	wait_queue.depth     = 2u;

	cr_assert_eq(sched_wake_all(&wait_queue), 1u, "terminated waiters must not stop wake_all");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "wake_all must consume the full queue");
	cr_assert_eq(live.state, THREAD_STATE_READY, "live waiter after a terminated entry must become ready");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "the live waiter must be queued to run");

	reset_test_state();
}

Test(sched, wake_one_is_exactly_once_for_a_pending_waiter) {
	struct thread            waiter;
	struct thread_wait_queue queue;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&waiter, "waiter", 0x314000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	thread_wait_queue_init(&queue);

	thread_mark_blocked(&waiter, THREAD_BLOCK_WAIT_QUEUE);
	waiter.blocked_queue = &queue;
	waiter.wait_status   = THREAD_WAIT_STATUS_PENDING;
	queue.head           = &waiter;
	queue.tail           = &waiter;
	queue.depth          = 1u;

	cr_assert(sched_wake_one(&queue), "first wake must succeed");
	cr_assert_not(sched_wake_one(&queue), "second wake must not duplicate the waiter");
	cr_assert_eq(thread_wait_queue_depth(&queue), 0u, "wait queue must be empty");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "waiter must exist exactly once in run queue");
	cr_assert(thread_is_queued(&waiter), "woken waiter must be queued");

	sched_regression_reset();
}
