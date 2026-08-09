#include "test_support.h"

Test(sched, sleep_until_tick_blocks_and_wakes_on_deadline) {
	const struct thread_create_params sleeper_params = {
		.name              = "sleeper",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x340000u,
		.kernel_stack_top  = 0x344000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params worker_params = {
		.name              = "worker",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x350000u,
		.kernel_stack_top  = 0x354000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread sleeper;
	struct thread worker;
	uint64_t      deadline_tick;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&sleeper, &sleeper_params), "thread_init failed for sleeper thread");
	cr_assert(thread_init(&worker, &worker_params), "thread_init failed for worker thread");
	sched_test_set_one_tick_timeslice(&worker);
	cr_assert(sched_make_runnable(&sleeper), "failed to make sleeper runnable");
	cr_assert(sched_make_runnable(&worker), "failed to make worker runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should dispatch first");

	deadline_tick = sched_tick_count() + 2u;
	cr_assert(sched_sleep_until_tick(deadline_tick), "sched_sleep_until_tick failed");
	cr_assert_eq(sleeper.state, THREAD_STATE_BLOCKED, "sleeper should block while sleeping");
	cr_assert_eq(sleeper.block_reason, THREAD_BLOCK_SLEEP, "sleeper block reason should be sleep");
	cr_assert_eq(sched_current_thread(), &worker, "worker should run after sleeper blocks");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "run queue should be empty after dispatching worker");

	sched_tick();
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "sleeper should not wake before deadline");
	cr_assert(!sched_handle_interrupt_exit(), "worker should keep running while no other thread is runnable");

	sched_tick();
	cr_assert_eq(sleeper.state, THREAD_STATE_READY, "sleeper should be ready after deadline");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "sleeper should be queued after wake");
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should preempt the worker once the sleeper wakes");
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should run after being woken");

	reset_test_state();
}

Test(sched, pending_interrupt_aborts_interruptible_wait_handoff) {
	const struct thread_create_params params = {
		.name              = "interrupted_waiter",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x400000u,
		.kernel_stack_top  = 0x404000u,
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
	cr_assert(thread_init(&current, &params), "thread_init failed for interrupted waiter");
	sched_set_current(cpu_current(), &current);

	thread_request_interrupt(&current);
	cr_assert(sched_interrupt_thread(&current), "interrupt request should succeed");
	cr_assert(thread_interrupt_pending(&current), "interrupt request should remain pending");
	wait_state = spinlock_lock_irqsave(&wait_queue.lock);
	cr_assert_eq(sched_block_current_interruptible_locked(&wait_queue, THREAD_BLOCK_SIGNAL, wait_state),
	             SCHED_BLOCK_INTERRUPTED,
	             "a pending upcall must abort the wait handoff");
	cr_assert_eq(current.state, THREAD_STATE_RUNNING, "interrupted waiter must remain running");
	cr_assert_null(current.blocked_queue, "interrupted waiter must not retain a blocked queue");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "interrupted waiter must not enter the wait queue");
	thread_clear_interrupt(&current);

	reset_test_state();
}

Test(sched, interrupt_during_block_handoff_requeues_current_thread) {
	const struct thread_create_params params = {
		.name              = "handoff_interrupted",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x410000u,
		.kernel_stack_top  = 0x414000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread            current;
	struct thread_wait_queue wait_queue;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	thread_wait_queue_init(&wait_queue);
	cr_assert(thread_init(&current, &params), "thread_init failed for interrupted handoff");
	sched_set_current(cpu_current(), &current);

	thread_mark_blocked(&current, THREAD_BLOCK_SIGNAL);
	current.blocked_queue = &wait_queue;
	current.wait_status   = THREAD_WAIT_STATUS_PENDING;
	(void)__atomic_fetch_or(&current.flags, (uint32_t)THREAD_FLAG_WAIT_INTERRUPTIBLE, __ATOMIC_ACQ_REL);
	wait_queue.head  = &current;
	wait_queue.tail  = &current;
	wait_queue.depth = 1u;

	thread_request_interrupt(&current);
	cr_assert(sched_interrupt_thread(&current), "interrupt during block handoff must succeed");
	cr_assert_eq(thread_wait_queue_depth(&wait_queue), 0u, "interrupted handoff must leave the wait queue");
	cr_assert_eq(current.state, THREAD_STATE_READY, "interrupted handoff must become ready");
	cr_assert(thread_is_queued(&current), "interrupted handoff must remain runnable");
	cr_assert_eq(current.wait_status, THREAD_WAIT_STATUS_INTERRUPTED, "interrupt result must remain observable");
	cr_assert(thread_interrupt_pending(&current), "interrupt must remain pending until upcall delivery");
	thread_clear_interrupt(&current);

	reset_test_state();
}
