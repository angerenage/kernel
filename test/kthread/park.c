#include "test_support.h"

Test(kthread, park_blocks_until_another_thread_unparks_the_current_thread) {
	const struct thread_create_params parker_params = {
		.name              = "parker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x304000u,
		.kernel_stack_top  = 0x308000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner_params = {
		.name              = "runner",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x308000u,
		.kernel_stack_top  = 0x30c000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread parker = {.stack_id = VMM_ID_INVALID};
	struct thread  runner;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&parker.thread, &parker_params), "thread_init failed for parker");
	cr_assert(thread_init(&runner, &runner_params), "thread_init failed for runner");
	cr_assert(sched_make_runnable(&runner), "runner should be runnable while the parker is blocked");

	sched_set_current(cpu_current(), &parker.thread);
	kthread_test_park_target     = &parker;
	kthread_test_park_hook_armed = true;
	hal_cpu_mock_set_context_switch_hook(kthread_test_park_context_switch_hook);
	cr_assert(kthread_park(), "kthread_park should return after another thread unparks the target");

	cr_assert_eq(kthread_test_park_hook_runs, 1u, "park hook should run exactly once");
	cr_assert_eq(sched_current_thread(), &parker.thread, "parker should resume once unparked");
	cr_assert_eq(parker.thread.state, THREAD_STATE_RUNNING, "parker should be running after park returns");
	cr_assert_eq(parker.thread.block_reason, THREAD_BLOCK_NONE, "park should clear the parked block reason");
	cr_assert_eq(thread_wait_queue_depth(&parker.thread.park_wait_queue), 0u, "park wait queue should be empty");
	cr_assert_eq(parker.thread.flags & THREAD_FLAG_PARK_PERMIT,
	             0u,
	             "waking a parked thread should consume the delivered park permit");

	reset_test_state();
}

Test(kthread, unpark_before_park_leaves_a_single_permit) {
	const struct thread_create_params worker_params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30c000u,
		.kernel_stack_top  = 0x310000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&worker.thread, &worker_params), "thread_init failed for worker");
	sched_set_current(cpu_current(), &worker.thread);

	hal_cpu_mock_set_context_switch_hook(kthread_test_park_context_switch_hook);
	cr_assert(kthread_unpark(&worker), "kthread_unpark should grant a permit to a running thread");
	cr_assert(kthread_park(), "kthread_park should consume a previously granted permit without blocking");

	cr_assert_eq(kthread_test_park_hook_runs, 0u, "consuming an existing permit should not context switch");
	cr_assert_eq(sched_current_thread(), &worker.thread, "worker should keep the CPU when park consumes a permit");
	cr_assert_eq(worker.thread.flags & THREAD_FLAG_PARK_PERMIT, 0u, "kthread_park should consume the stored permit");
	cr_assert_eq(thread_wait_queue_depth(&worker.thread.park_wait_queue), 0u, "park should not queue the worker");

	reset_test_state();
}

Test(kthread, repeated_unpark_coalesces_to_one_binary_permit) {
	struct kthread target;

	kthread_test_init_scheduler();
	kthread_test_init_target(&target, "parker", 0x920000u, true);

	cr_assert(kthread_unpark(&target));
	cr_assert(kthread_unpark(&target));
	cr_assert((target.thread.flags & THREAD_FLAG_PARK_PERMIT) != 0u, "repeated unpark must leave one available permit");

	cr_assert(kthread_park(), "park must consume the available permit without blocking");
	cr_assert_eq(target.thread.flags & THREAD_FLAG_PARK_PERMIT, 0u, "one park must consume the coalesced permit");
	cr_assert_eq(
		thread_wait_queue_depth(&target.thread.park_wait_queue), 0u, "permit consumption must not enqueue the thread");

	reset_test_state();
}

Test(kthread, unpark_only_changes_the_park_permit_flag) {
	struct kthread target;
	const uint32_t persistent =
		THREAD_FLAG_CANCEL_PENDING | THREAD_FLAG_CANCEL_DISABLED | THREAD_FLAG_INTERRUPT_PENDING;

	kthread_test_init_scheduler();
	kthread_test_init_target(&target, "parker", 0x920000u, true);
	target.thread.flags |= persistent;

	cr_assert(kthread_unpark(&target));
	cr_assert_eq(
		target.thread.flags & persistent, persistent, "unpark must preserve unrelated persistent thread flags");
	cr_assert((target.thread.flags & THREAD_FLAG_PARK_PERMIT) != 0u, "unpark must grant the park permit");

	cr_assert(kthread_park(), "park must consume an available permit");
	cr_assert_eq(target.thread.flags & persistent, persistent, "park must preserve unrelated persistent thread flags");
	cr_assert_eq(target.thread.flags & THREAD_FLAG_PARK_PERMIT, 0u, "park must consume the available permit");

	reset_test_state();
}
