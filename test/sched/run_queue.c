#include "test_support.h"

Test(sched, duplicate_make_runnable_is_rejected_without_queue_mutation) {
	struct thread worker;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&worker, "worker", 0x300000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);

	cr_assert(sched_make_runnable(&worker), "first make_runnable failed");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "first enqueue depth mismatch");
	cr_assert_not(sched_make_runnable(&worker), "second make_runnable must reject duplicate placement");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "duplicate placement must not change depth");
	cr_assert(thread_is_queued(&worker), "duplicate placement must not clear queued state");

	sched_regression_reset();
}

Test(sched, remove_and_requeue_preserve_exactly_one_runnable_instance) {
	struct thread worker;

	sched_regression_init_single_cpu();
	sched_regression_init_thread(&worker, "worker", 0x304000u, THREAD_PRIORITY_DEFAULT, cpu_current(), NULL);

	cr_assert(sched_make_runnable(&worker), "make_runnable failed");
	cr_assert(sched_remove_runnable(&worker), "remove_runnable failed");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "remove must consume the queued instance");
	cr_assert_not(thread_is_queued(&worker), "removed thread must not remain queued");
	cr_assert_not(sched_remove_runnable(&worker), "second remove must be side-effect free");
	cr_assert(sched_make_runnable(&worker), "removed thread must be queueable again");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "requeue must create one runnable instance");

	sched_regression_reset();
}
