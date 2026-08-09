#include "test_support.h"

Test(sched, per_cpu_stats_track_sampled_kernel_idle_and_thread_time) {
	const struct thread_create_params worker_params = {
		.name              = "worker",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x31d000u,
		.kernel_stack_top  = 0x321000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread          worker;
	struct sched_cpu_stats stats;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");

	sched_tick();
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	sched_tick();

	cr_assert(thread_init(&worker, &worker_params), "thread_init failed for worker thread");
	sched_set_current(cpu_current(), &worker);
	sched_tick();

	cr_assert(sched_get_cpu_stats(cpu_current(), &stats), "sched_get_cpu_stats failed");
	cr_assert_eq(stats.total_ticks, 3u, "per-CPU total tick accounting mismatch");
	cr_assert_eq(stats.kernel_ticks, 1u, "kernel tick accounting mismatch");
	cr_assert_eq(stats.idle_ticks, 1u, "idle tick accounting mismatch");
	cr_assert_eq(stats.thread_ticks, 1u, "thread tick accounting mismatch");

	reset_test_state();
}

Test(sched, per_cpu_stats_track_local_switch_preempt_and_yield_counts) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x321000u,
		.kernel_stack_top  = 0x325000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x325000u,
		.kernel_stack_top  = 0x329000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread          first;
	struct thread          second;
	struct sched_cpu_stats stats;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	sched_test_set_one_tick_timeslice(&first);
	sched_test_set_one_tick_timeslice(&second);

	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first thread should dispatch after yield");

	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");
	sched_tick();
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume the pending reschedule");

	cr_assert(sched_get_cpu_stats(cpu_current(), &stats), "sched_get_cpu_stats failed");
	cr_assert_eq(stats.yield_count, 1u, "per-CPU yield counter mismatch");
	cr_assert_eq(stats.timeslice_preempt_count, 1u, "per-CPU preempt counter mismatch");
	cr_assert_eq(stats.context_switch_count, 2u, "per-CPU context switch counter mismatch");

	reset_test_state();
}
