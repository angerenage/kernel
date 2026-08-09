#include "test_support.h"

Test(sched, runnable_threads_yield_in_fifo_order) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");

	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "run queue depth mismatch after enqueue");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "idle CPU should dispatch first runnable thread");
	cr_assert_eq(first.state, THREAD_STATE_RUNNING, "first thread should be running after first yield");
	cr_assert_eq(first.cpu, cpu_current(), "first thread bound to wrong CPU");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "run queue depth mismatch after first dispatch");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &second, "yield should advance to the next runnable thread");
	cr_assert_eq(second.state, THREAD_STATE_RUNNING, "second thread should be running after second yield");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "run queue depth mismatch after second dispatch");
	cr_assert(thread_is_queued(&first), "first thread should have been re-queued behind second");

	cr_assert(sched_remove_runnable(&first), "sched_remove_runnable should unlink the queued thread");
	cr_assert(!thread_is_queued(&first), "removed runnable thread should no longer be queued");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "run queue should be empty after removing first");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &second, "single runnable thread should keep the CPU after yielding");
	cr_assert_eq(second.state, THREAD_STATE_RUNNING, "second thread should still be running");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "single-thread yield should leave run queue empty");
	cr_assert(!sched_make_runnable(sched_idle_thread(cpu_current())), "idle thread must not be runnable");

	reset_test_state();
}

Test(sched, runnable_threads_dispatch_highest_priority_first) {
	const struct thread_create_params low_params = {
		.name              = "low",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x302000u,
		.kernel_stack_top  = 0x306000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT - 4,
		.detached          = false,
	};
	const struct thread_create_params high_params = {
		.name              = "high",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x306000u,
		.kernel_stack_top  = 0x30a000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 4,
		.detached          = false,
	};
	struct thread low;
	struct thread high;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&low, &low_params), "thread_init failed for low-priority thread");
	cr_assert(thread_init(&high, &high_params), "thread_init failed for high-priority thread");

	cr_assert(sched_make_runnable(&low), "failed to make low-priority thread runnable");
	cr_assert(sched_make_runnable(&high), "failed to make high-priority thread runnable");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "run queue depth mismatch after enqueue");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &high, "highest-priority runnable thread should dispatch first");
	cr_assert(thread_is_queued(&low), "lower-priority thread should remain queued");

	reset_test_state();
}

Test(sched, dispatch_activates_thread_address_space) {
	struct address_space user_space = {
		.hal_space   = {.lower_root_phys = 0x4242000u},
		.initialized = true,
	};
	const struct thread_create_params user_params = {
		.name              = "user",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x305000u,
		.kernel_stack_top  = 0x309000u,
		.address_space     = &user_space,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread user_thread;

	init_bound_bootstrap_cpu();
	hal_paging_mock_reset_active();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert_eq(
		hal_paging_mock_active_root_phys(), 0u, "idle startup should not activate an uninitialized kernel space");

	cr_assert(thread_init(&user_thread, &user_params), "thread_init failed for userspace thread");
	cr_assert(sched_make_runnable(&user_thread), "failed to make userspace thread runnable");
	sched_yield();

	cr_assert_eq(sched_current_thread(), &user_thread, "userspace thread should dispatch");
	cr_assert_eq(hal_paging_mock_active_root_phys(),
	             user_space.hal_space.lower_root_phys,
	             "dispatch should activate the userspace paging root");

	reset_test_state();
}

Test(sched, dynamic_priority_changes_reorder_queued_and_running_threads) {
	struct thread first;
	struct thread second;
	struct thread third;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&first, "first", 0x308000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_regression_init_thread(&second, "second", 0x30c000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_regression_init_thread(&third, "third", 0x310000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);

	cr_assert(sched_make_runnable(&first), "first enqueue failed");
	cr_assert(sched_make_runnable(&second), "second enqueue failed");
	cr_assert(sched_make_runnable(&third), "third enqueue failed");
	sched_set_thread_effective_priority(&second, THREAD_PRIORITY_DEFAULT + 5);
	sched_set_thread_effective_priority(&third, THREAD_PRIORITY_DEFAULT + 5);

	sched_yield();
	cr_assert_eq(sched_current_thread(), &second, "first boosted FIFO peer must run first");
	cr_assert(thread_is_queued(&third), "second boosted peer must remain queued");
	cr_assert(thread_is_queued(&first), "lower-priority peer must remain queued");

	sched_set_thread_effective_priority(&second, THREAD_PRIORITY_MIN);
	cr_assert(sched_reschedule_pending(cpu_current()), "demoting current below queued work must request preemption");
	cr_assert(sched_handle_interrupt_exit(), "pending priority preemption must be consumed");
	cr_assert_eq(sched_current_thread(), &third, "higher-priority queued peer must preempt demoted current");
	cr_assert(thread_is_queued(&second), "demoted current must remain runnable");

	sched_regression_reset();
}

Test(sched, reschedule_requested_during_context_switch_remains_pending) {
	struct thread first;
	struct thread second;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&first, "first", 0x320000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_regression_init_thread(&second, "second", 0x324000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_set_current(cpu_current(), &first);
	cr_assert(sched_make_runnable(&second), "second enqueue failed");

	sched_regression_reschedule_hook_armed = true;
	hal_cpu_mock_set_context_switch_hook(sched_regression_reschedule_hook);
	sched_request_reschedule(cpu_current());
	cr_assert(sched_handle_interrupt_exit(), "initial reschedule request must switch threads");
	cr_assert_eq(sched_current_thread(), &second, "second thread must become current");
	cr_assert(sched_reschedule_pending(cpu_current()),
	          "a request raised during the switch must survive the request being consumed");

	sched_regression_reset();
}

Test(sched, accepted_thread_is_never_orphaned_when_address_space_activation_fails) {
	struct address_space unusable_space = {0};
	struct thread        current;
	struct thread        candidate;
	bool                 accepted;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&current, "current", 0x330000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_regression_init_thread(
		&candidate, "candidate", 0x334000u, THREAD_PRIORITY_DEFAULT, cpu_current(), &unusable_space);
	sched_set_current(cpu_current(), &current);

	accepted = sched_make_runnable(&candidate);
	if (accepted) {
		sched_yield();
		cr_assert(sched_current_thread() == &candidate || thread_is_queued(&candidate),
		          "an accepted runnable thread must not disappear after dispatch failure");
		if (sched_current_thread() == &current) {
			cr_assert_eq(current.state,
			             THREAD_STATE_RUNNING,
			             "current descriptor must remain RUNNING when dispatch cannot replace it");
			cr_assert_not(thread_is_queued(&current),
			              "current thread must not simultaneously remain current and queued");
		}
	}

	sched_regression_reset();
}

Test(sched, current_thread_kernel_entry_stack_tracks_dispatch_target) {
	struct thread worker;
	struct cpu*   cpu;

	sched_regression_init_single_cpu();
	cpu = cpu_current();
	sched_regression_init_thread(&worker, "worker", 0x33c000u, THREAD_PRIORITY_DEFAULT, cpu, NULL);

	cr_assert_eq(cpu->kernel_entry_stack_top, cpu->boot_stack_top, "idle scheduler baseline must use the boot stack");
	cr_assert(sched_make_runnable(&worker), "worker enqueue failed");
	sched_yield();
	cr_assert_eq(sched_current_thread(), &worker, "worker must dispatch");
	cr_assert_eq(
		cpu->kernel_entry_stack_top, worker.kernel_stack_top, "kernel entry stack must follow the current thread");

	sched_regression_reset();
}
