#include "test_support.h"

Test(sched, make_runnable_prefers_preferred_cpu_over_lower_depth_target) {
	const struct thread_create_params pinned_params = {
		.name              = "pinned_existing",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x304000u,
		.kernel_stack_top  = 0x308000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread pinned;
	struct thread preferred;

	init_started_dual_cpu_topology(&bsp, &ap);

	{
		const struct thread_create_params preferred_params = {
			.name              = "preferred",
			.entry             = sched_test_thread_entry,
			.arg               = NULL,
			.kernel_stack_base = 0x308000u,
			.kernel_stack_top  = 0x30c000u,
			.preferred_cpu     = ap,
			.detached          = false,
		};

		cr_assert(thread_init(&pinned, &pinned_params), "thread_init failed for pinned thread");
		pinned.preferred_cpu = ap;
		cr_assert(thread_init(&preferred, &preferred_params), "thread_init failed for preferred thread");
	}

	cr_assert(sched_make_runnable(&pinned), "failed to enqueue existing AP thread");
	cr_assert_eq(pinned.cpu, ap, "pinned thread should land on AP");
	cr_assert_eq(sched_run_queue_depth(bsp), 0u, "BSP should still have an empty run queue");
	cr_assert_eq(sched_run_queue_depth(ap), 1u, "AP should have one queued thread before preferred enqueue");

	cr_assert(sched_make_runnable(&preferred), "failed to enqueue preferred thread");
	cr_assert_eq(preferred.cpu, ap, "preferred_cpu should override lower BSP queue depth");
	cr_assert_eq(sched_run_queue_depth(bsp), 0u, "BSP should remain empty when preferred_cpu selects AP");
	cr_assert_eq(sched_run_queue_depth(ap), 2u, "AP should contain both preferred threads");

	reset_test_state();
}

Test(sched, make_runnable_chooses_cpu_with_smallest_run_queue_depth) {
	const struct thread_create_params busy_params = {
		.name              = "busy",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30c000u,
		.kernel_stack_top  = 0x310000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params balanced_params = {
		.name              = "balanced",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread busy;
	struct thread balanced;

	init_started_dual_cpu_topology(&bsp, &ap);

	cr_assert(thread_init(&busy, &busy_params), "thread_init failed for busy thread");
	busy.preferred_cpu = bsp;
	cr_assert(thread_init(&balanced, &balanced_params), "thread_init failed for balanced thread");

	cr_assert(sched_make_runnable(&busy), "failed to enqueue busy thread");
	cr_assert_eq(busy.cpu, bsp, "busy thread should be pinned to BSP");
	cr_assert_eq(sched_run_queue_depth(bsp), 1u, "BSP should have one queued thread");
	cr_assert_eq(sched_run_queue_depth(ap), 0u, "AP should still be empty");

	cr_assert(sched_make_runnable(&balanced), "failed to enqueue balanced thread");
	cr_assert_eq(balanced.cpu, ap, "unbound thread should pick the less-loaded AP run queue");
	cr_assert_eq(sched_run_queue_depth(bsp), 1u, "BSP queue depth should remain unchanged");
	cr_assert_eq(sched_run_queue_depth(ap), 1u, "AP queue depth should increase after balancing");

	reset_test_state();
}

Test(sched, make_runnable_prefers_idle_cpu_over_busy_current_cpu) {
	const struct thread_create_params busy_params = {
		.name              = "busy_running",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x312000u,
		.kernel_stack_top  = 0x316000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params balanced_params = {
		.name              = "balanced_idle_target",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x316000u,
		.kernel_stack_top  = 0x31a000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread busy;
	struct thread balanced;

	init_started_dual_cpu_topology(&bsp, &ap);

	cr_assert(thread_init(&busy, &busy_params), "thread_init failed for busy running thread");
	cr_assert(thread_init(&balanced, &balanced_params), "thread_init failed for balanced thread");

	cpu_bind_current(bsp);
	sched_set_current(bsp, &busy);

	cr_assert(sched_make_runnable(&balanced), "failed to enqueue balanced thread");
	cr_assert_eq(balanced.cpu, ap, "idle AP should be preferred over a CPU already running work");
	cr_assert_eq(sched_run_queue_depth(bsp), 0u, "busy BSP should not receive additional queued work");
	cr_assert_eq(sched_run_queue_depth(ap), 1u, "idle AP should receive the queued work");

	reset_test_state();
}

Test(sched, remote_enqueue_requests_reschedule_and_kicks_target_cpu) {
	const struct thread_create_params remote_params = {
		.name              = "remote",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x314000u,
		.kernel_stack_top  = 0x318000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread remote;

	init_started_dual_cpu_topology(&bsp, &ap);
	hal_cpu_mock_reset_kicks();

	cr_assert(thread_init(&remote, &remote_params), "thread_init failed for remote thread");
	remote.preferred_cpu = ap;

	cr_assert(sched_make_runnable(&remote), "failed to enqueue remote thread");
	cr_assert_eq(remote.cpu, ap, "remote thread should land on the AP run queue");
	cr_assert(sched_reschedule_pending(ap), "remote enqueue should request an AP reschedule");
	cr_assert_eq(hal_cpu_mock_kick_count(ap), 1u, "remote enqueue should kick the AP once");
	cr_assert_eq(hal_cpu_mock_kick_count(bsp), 0u, "remote enqueue should not kick the BSP");

	reset_test_state();
}

Test(sched, remote_duplicate_enqueue_does_not_duplicate_kicks_or_queue_entries) {
	struct cpu*   bsp;
	struct cpu*   ap;
	struct thread remote;

	sched_regression_init_dual_cpu(&bsp, &ap);
	hal_cpu_mock_reset_kicks();
	sched_regression_init_thread(&remote, "remote", 0x31c000u, THREAD_PRIORITY_DEFAULT, ap, NULL);

	cr_assert(sched_make_runnable(&remote), "remote enqueue failed");
	cr_assert_not(sched_make_runnable(&remote), "duplicate remote enqueue must be rejected");
	cr_assert_eq(remote.cpu, ap, "remote thread CPU mismatch");
	cr_assert_eq(sched_run_queue_depth(ap), 1u, "remote queue must contain one instance");
	cr_assert_eq(hal_cpu_mock_kick_count(ap), 1u, "duplicate enqueue must not emit a second kick");
	cr_assert_eq(hal_cpu_mock_kick_count(bsp), 0u, "BSP must not be kicked");

	sched_regression_reset();
}
