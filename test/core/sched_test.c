#include <core/cpu.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

uintptr_t hal_paging_mock_active_root_phys(void);
void      hal_paging_mock_reset_active(void);

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

static void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
	hal_paging_mock_reset_active();
}

static void sched_test_thread_entry(void* arg) {
	(void)arg;
}

static void sched_test_set_one_tick_timeslice(struct thread* thread) {
	if (thread == NULL) return;

	thread->timeslice_ticks     = 1u;
	thread->timeslice_remaining = 1u;
}

static void init_started_dual_cpu_topology(struct cpu** out_bsp, struct cpu** out_ap) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 10u,
         .arch_id         = 0x20u,
         .role            = CPU_ROLE_BSP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
		{
         .index           = 1u,
         .processor_id    = 11u,
         .arch_id         = 0x21u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x210000u,
         .boot_stack_top  = 0x214000u,
		 },
	};
	struct cpu* bsp;
	struct cpu* ap;

	cr_assert(cpu_topology_init(init_info, 2u, 0u), "cpu_topology_init failed");

	bsp = cpu_bsp();
	ap  = cpu_by_index(1u);
	cr_assert_not_null(bsp, "cpu_bsp returned NULL");
	cr_assert_not_null(ap, "cpu_by_index returned NULL for AP");

	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(bsp), "sched_start_cpu failed for BSP");

	cpu_bind_current(ap);
	cpu_interrupts_set_ready(ap, false);
	cr_assert(sched_start_cpu(ap), "sched_start_cpu failed for AP");

	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);

	if (out_bsp != NULL) *out_bsp = bsp;
	if (out_ap != NULL) *out_ap = ap;
}

Test(sched, init_creates_per_cpu_idle_threads) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 10u,
         .arch_id         = 0x20u,
         .role            = CPU_ROLE_BSP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
		{
         .index           = 1u,
         .processor_id    = 11u,
         .arch_id         = 0x21u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x210000u,
         .boot_stack_top  = 0x214000u,
		 },
	};
	struct cpu*    bsp;
	struct cpu*    ap;
	struct thread* bsp_idle;
	struct thread* ap_idle;

	cr_assert(cpu_topology_init(init_info, 2u, 0u), "cpu_topology_init failed");
	bsp = cpu_bsp();
	ap  = cpu_by_index(1u);
	cr_assert_not_null(bsp, "cpu_bsp returned NULL");
	cr_assert_not_null(ap, "cpu_by_index returned NULL for AP");
	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(cpu_current(), false);

	cr_assert(sched_init(), "sched_init failed");

	bsp_idle = sched_idle_thread(bsp);
	ap_idle  = sched_idle_thread(ap);
	cr_assert_not_null(bsp_idle, "BSP idle thread missing");
	cr_assert_not_null(ap_idle, "AP idle thread missing");
	cr_assert(thread_is_idle(bsp_idle), "BSP idle thread missing idle flag");
	cr_assert(thread_is_idle(ap_idle), "AP idle thread missing idle flag");
	cr_assert_eq(bsp_idle->cpu, bsp, "BSP idle thread bound to wrong CPU");
	cr_assert_eq(ap_idle->cpu, ap, "AP idle thread bound to wrong CPU");
	cr_assert_str_eq(bsp_idle->name, "idle/0", "unexpected BSP idle thread name");
	cr_assert_str_eq(ap_idle->name, "idle/1", "unexpected AP idle thread name");
	cr_assert_eq(sched_run_queue_depth(bsp), 0u, "fresh BSP run queue should be empty");
	cr_assert_eq(sched_run_queue_depth(ap), 0u, "fresh AP run queue should be empty");

	cr_assert(sched_start_cpu(bsp), "sched_start_cpu failed for BSP");
	cr_assert_eq(sched_current_thread(), bsp_idle, "BSP current thread should start at idle");

	reset_test_state();
}

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

Test(sched, high_priority_thread_preempts_lower_priority_current_at_interrupt_exit) {
	const struct thread_create_params low_params = {
		.name              = "low_current",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30a000u,
		.kernel_stack_top  = 0x30e000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT - 2,
		.detached          = false,
	};
	const struct thread_create_params high_params = {
		.name              = "high_ready",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x30e000u,
		.kernel_stack_top  = 0x312000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 2,
		.detached          = false,
	};
	struct thread low;
	struct thread high;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&low, &low_params), "thread_init failed for low-priority current thread");
	cr_assert(thread_init(&high, &high_params), "thread_init failed for high-priority runnable thread");

	sched_set_current(cpu_current(), &low);
	cr_assert(sched_make_runnable(&high), "failed to make high-priority thread runnable");
	cr_assert(sched_reschedule_pending(cpu_current()), "higher-priority enqueue should request deferred preemption");
	cr_assert_eq(sched_current_thread(), &low, "preemption should wait for interrupt-exit handling");

	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume the priority preemption request");
	cr_assert_eq(sched_current_thread(), &high, "high-priority thread should preempt lower-priority current thread");
	cr_assert(thread_is_queued(&low), "preempted lower-priority thread should be re-queued");

	reset_test_state();
}

Test(sched, timeslice_expiry_rotates_equal_priority_threads_only) {
	const struct thread_create_params high_params = {
		.name              = "high_current",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x312000u,
		.kernel_stack_top  = 0x316000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 3,
		.detached          = false,
	};
	const struct thread_create_params low_params = {
		.name              = "low_queued",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x316000u,
		.kernel_stack_top  = 0x31a000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT - 3,
		.detached          = false,
	};
	const struct thread_create_params peer_params = {
		.name              = "high_peer",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x31a000u,
		.kernel_stack_top  = 0x31e000u,
		.preferred_cpu     = NULL,
		.base_priority     = THREAD_PRIORITY_DEFAULT + 3,
		.detached          = false,
	};
	struct thread high;
	struct thread low;
	struct thread peer;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&high, &high_params), "thread_init failed for high-priority current thread");
	cr_assert(thread_init(&low, &low_params), "thread_init failed for low-priority queued thread");
	cr_assert(thread_init(&peer, &peer_params), "thread_init failed for high-priority peer thread");
	sched_test_set_one_tick_timeslice(&high);
	sched_test_set_one_tick_timeslice(&low);
	sched_test_set_one_tick_timeslice(&peer);

	sched_set_current(cpu_current(), &high);
	cr_assert(sched_make_runnable(&low), "failed to make low-priority thread runnable");
	sched_tick();
	cr_assert(!sched_reschedule_pending(cpu_current()),
	          "lower-priority queued work should not consume the current priority band's timeslice");
	cr_assert(!sched_handle_interrupt_exit(), "no preemption should be pending for lower-priority queued work");
	cr_assert_eq(sched_current_thread(), &high, "high-priority current thread should keep running");

	cr_assert(sched_make_runnable(&peer), "failed to make equal-priority peer runnable");
	sched_tick();
	cr_assert(sched_reschedule_pending(cpu_current()),
	          "equal-priority runnable work should rotate on timeslice expiry");
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume equal-priority timeslice rotation");
	cr_assert_eq(sched_current_thread(), &peer, "equal-priority peer should run after current timeslice expires");
	cr_assert(thread_is_queued(&high), "preempted equal-priority thread should be queued for round-robin");
	cr_assert(thread_is_queued(&low), "lower-priority thread should remain queued behind the priority band");

	reset_test_state();
}

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

Test(sched, timer_tick_requests_and_consumes_timeslice_preemption) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x315000u,
		.kernel_stack_top  = 0x319000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x319000u,
		.kernel_stack_top  = 0x31d000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread      first;
	struct thread      second;
	struct sched_stats stats_before;
	struct sched_stats stats_after;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	sched_test_set_one_tick_timeslice(&first);
	sched_test_set_one_tick_timeslice(&second);

	sched_set_current(cpu_current(), &first);
	cr_assert(sched_make_runnable(&second), "failed to make second runnable");
	sched_get_stats(&stats_before);

	sched_tick();
	cr_assert(sched_reschedule_pending(cpu_current()), "timeslice expiry should request a deferred reschedule");
	cr_assert_eq(sched_current_thread(), &first, "preemption should wait for interrupt-exit handling");

	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should consume the pending reschedule");
	cr_assert_eq(sched_current_thread(), &second, "interrupt exit should dispatch the next runnable thread");
	cr_assert(thread_is_queued(&first), "preempted thread should be re-queued");

	sched_get_stats(&stats_after);
	cr_assert_eq(stats_after.timeslice_preempt_count,
	             stats_before.timeslice_preempt_count + 1u,
	             "timeslice preemption counter should increment");
	cr_assert_eq(stats_after.context_switch_count,
	             stats_before.context_switch_count + 1u,
	             "context switch counter should increment");

	reset_test_state();
}

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

Test(sched, timer_preemption_rotates_non_yielding_workers) {
	const struct thread_create_params first_params = {
		.name              = "first",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x380000u,
		.kernel_stack_top  = 0x384000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "second",
		.entry             = sched_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x390000u,
		.kernel_stack_top  = 0x394000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;
	size_t        first_progress  = 0u;
	size_t        second_progress = 0u;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	cr_assert(thread_init(&first, &first_params), "thread_init failed for first worker");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second worker");
	sched_test_set_one_tick_timeslice(&first);
	sched_test_set_one_tick_timeslice(&second);
	cr_assert(sched_make_runnable(&first), "failed to make first worker runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second worker runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first worker should dispatch first");

	for (size_t i = 0; i < 6u; i++) {
		if (sched_current_thread() == &first) first_progress++;
		if (sched_current_thread() == &second) second_progress++;

		sched_tick();
		(void)sched_handle_interrupt_exit();
	}

	cr_assert_gt(first_progress, 0u, "first worker should make progress without yielding explicitly");
	cr_assert_gt(second_progress, 0u, "second worker should make progress without yielding explicitly");

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
