#include "test_support.h"

Test(kthread, current_start_and_yield_delegate_to_scheduler) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};
	struct thread* idle;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	idle = sched_idle_thread(cpu_current());

	cr_assert_eq(kthread_current(), idle, "kthread_current should match scheduler current thread");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	cr_assert(sched_make_runnable(&worker.thread), "sched_make_runnable should queue the worker thread");

	kthread_yield();
	cr_assert_eq(kthread_current(), &worker.thread, "kthread_yield should dispatch the runnable worker thread");

	reset_test_state();
}
