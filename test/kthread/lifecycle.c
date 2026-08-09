#include "test_support.h"

Test(kthread, join_detach_and_cancel_validate_inputs) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x320000u,
		.kernel_stack_top  = 0x324000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker      = {.stack_id = VMM_ID_INVALID};
	struct kthread idle_target = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	thread_init_idle(&idle_target.thread, cpu_current(), "idle/test");

	cr_assert(!kthread_join(NULL, NULL), "kthread_join should reject NULL targets");
	cr_assert(!kthread_timed_join(NULL, 0u, NULL), "kthread_timed_join should reject NULL targets");
	cr_assert(!kthread_unpark(NULL), "kthread_unpark should reject NULL targets");
	sched_set_current(cpu_current(), &worker.thread);
	cr_assert(!kthread_join(&worker, NULL), "kthread_join should reject self-join");
	cr_assert(!kthread_timed_join(&worker, 0u, NULL), "kthread_timed_join should reject self-join");
	sched_set_current(cpu_current(), &idle_target.thread);
	cr_assert(!kthread_unpark(&idle_target), "kthread_unpark should reject idle targets");
	cr_assert(thread_detach(&worker.thread), "thread_detach should succeed on live joinable thread");
	cr_assert(!kthread_join(&worker, NULL), "kthread_join should reject detached targets");
	cr_assert(!kthread_timed_join(&worker, 0u, NULL), "kthread_timed_join should reject detached targets");
	cr_assert(kthread_cancel(&worker), "kthread_cancel should set cancellation pending on live thread");
	cr_assert(thread_cancel_requested(&worker.thread), "cancel flag should be visible after kthread_cancel");
	thread_mark_exiting(&worker.thread, 1u);
	thread_mark_zombie(&worker.thread);
	cr_assert(!kthread_unpark(&worker), "kthread_unpark should reject terminated targets");

	reset_test_state();
}

Test(kthread, detach_live_thread_changes_ownership_contract_once) {
	struct kthread target;

	kthread_test_init_scheduler();
	kthread_test_init_target(&target, "target", 0x900000u, false);

	cr_assert(thread_is_joinable(&target.thread));
	cr_assert_null(target.thread.reap_callback);
	cr_assert(kthread_detach(&target), "first detach must succeed");
	cr_assert_not(thread_is_joinable(&target.thread), "detached thread must stop being joinable");
	cr_assert_not_null(target.thread.reap_callback, "detached kthread must install reap cleanup");
	cr_assert_eq(target.thread.reap_context, &target, "reap cleanup must retain the owning kthread");
	cr_assert_not(kthread_detach(&target), "second detach must be side-effect free");

	reset_test_state();
}

Test(kthread, detach_rejects_threads_already_in_terminal_transition) {
	struct kthread exiting;
	struct kthread zombie;

	kthread_test_init_scheduler();
	kthread_test_init_target(&exiting, "target", 0x904000u, false);
	kthread_test_init_target(&zombie, "target", 0x908000u, false);

	thread_mark_exiting(&exiting.thread, 11u);
	thread_mark_zombie(&zombie.thread);

	cr_assert_not(kthread_detach(&exiting), "EXITING thread must not become detached");
	cr_assert_not(kthread_detach(&zombie), "ZOMBIE thread must not become detached");

	reset_test_state();
}
