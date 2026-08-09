#include "test_support.h"

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
