#include <core/cpu.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

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
}

static void sched_test_thread_entry(void* arg) {
	(void)arg;
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
