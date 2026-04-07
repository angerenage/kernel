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

Test(sched, run_queue_is_fifo_and_falls_back_to_idle) {
	struct thread  first;
	struct thread  second;
	struct thread* next;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");

	thread_init(&first, "first", 0x300000u, 0x304000u);
	thread_init(&second, "second", 0x310000u, 0x314000u);

	cr_assert(sched_enqueue(cpu_current(), &first), "failed to enqueue first thread");
	cr_assert(sched_enqueue(cpu_current(), &second), "failed to enqueue second thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "run queue depth mismatch after enqueue");

	next = sched_dequeue_next(cpu_current());
	cr_assert_eq(next, &first, "scheduler did not preserve FIFO order for first dequeue");
	cr_assert_eq(next->state, THREAD_STATE_RUNNING, "first dequeued thread not marked running");
	cr_assert_eq(next->cpu, cpu_current(), "first dequeued thread bound to wrong CPU");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "run queue depth mismatch after first dequeue");

	next = sched_dequeue_next(cpu_current());
	cr_assert_eq(next, &second, "scheduler did not preserve FIFO order for second dequeue");
	cr_assert_eq(next->state, THREAD_STATE_RUNNING, "second dequeued thread not marked running");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "run queue depth mismatch after second dequeue");

	next = sched_dequeue_next(cpu_current());
	cr_assert_eq(next, sched_idle_thread(cpu_current()), "empty run queue should fall back to idle thread");
	cr_assert(!sched_enqueue(cpu_current(), next), "idle thread must not be enqueued");

	reset_test_state();
}
