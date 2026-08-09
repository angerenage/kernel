#include "test_support.h"

const struct cpu_init_info cpu_test_valid_topology[2] = {
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

void cpu_test_init_bound_bootstrap(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

void cpu_test_reset(void) {
	irq_enable_local();
	hal_cpu_local_bind(NULL);
}
