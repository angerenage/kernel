#include "test_support.h"

Test(sched, deferred_reap_publishes_zombie_and_invokes_callback_once) {
	struct thread exiting;
	struct thread successor;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&exiting, "exiting", 0x328000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	sched_regression_init_thread(&successor, "successor", 0x32c000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);
	thread_set_reap_callback(&exiting, sched_regression_reap_callback, NULL);
	sched_set_current(cpu_current(), &exiting);
	cr_assert(sched_make_runnable(&successor), "successor enqueue failed");

	thread_mark_exiting(&exiting, 0x55u);
	sched_yield();
	cr_assert_eq(sched_current_thread(), &successor, "scheduler must leave the exiting thread");
	cr_assert_eq(exiting.state, THREAD_STATE_ZOMBIE, "switched-out exiting thread must become zombie");
	cr_assert_eq(exiting.exit_code, 0x55u, "exit code must survive deferred reap");
	cr_assert_eq(sched_regression_reap_count, 1u, "reap callback must run once");

	sched_finish_context_switch();
	sched_complete_context_switch();
	cr_assert_eq(sched_regression_reap_count, 1u, "reap callback must remain exactly-once");

	sched_regression_reset();
}
