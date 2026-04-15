#include <core/cpu.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stdint.h>

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

Test(cpu_irq, topology_rejects_invalid_inputs_and_bootstrap_accessors_work) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 1u,
         .arch_id         = 2u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
	};

	cr_assert(!cpu_topology_init(NULL, 1u, 0u), "cpu_topology_init accepted NULL init_info");
	cr_assert(!cpu_topology_init(init_info, 0u, 0u), "cpu_topology_init accepted an empty topology");
	cr_assert(!cpu_topology_init(init_info, 1u, 1u), "cpu_topology_init accepted an out-of-range BSP index");
	cr_assert(!cpu_topology_init(init_info, 65u, 0u), "cpu_topology_init accepted too many CPUs");

	init_bound_bootstrap_cpu();

	cr_assert_eq(cpu_count(), 1u, "bootstrap topology should expose one CPU");
	cr_assert_not_null(cpu_current(), "cpu_current returned NULL after binding");
	cr_assert_eq(cpu_current(), cpu_bsp(), "cpu_current should match the bootstrap CPU");
	cr_assert_eq(cpu_index(), 0u, "bootstrap CPU index mismatch");
	cr_assert_eq(cpu_arch_id(), 0u, "hosted bootstrap arch id should come from the HAL mock");
	cr_assert(cpu_is_bsp(), "current CPU should be the BSP");
	cr_assert_eq(cpu_current()->boot_stack_base, 0x100000u, "bootstrap stack base mismatch");
	cr_assert_eq(cpu_current()->boot_stack_top, 0x104000u, "bootstrap stack top mismatch");
	cr_assert_null(cpu_by_index(1u), "cpu_by_index accepted an out-of-range index");

	reset_test_state();
}

Test(cpu_irq, state_transitions_track_online_count_without_double_counting) {
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
	cr_assert_not_null(ap, "cpu_by_index returned NULL for the AP");
	cr_assert_eq(cpu_online_count(), 0u, "online count should start at zero");

	cr_assert(cpu_set_state(bsp, CPU_STATE_ONLINE), "cpu_set_state failed for BSP");
	cr_assert_eq(cpu_state_get(bsp), CPU_STATE_ONLINE, "BSP state did not become ONLINE");
	cr_assert_eq(cpu_online_count(), 1u, "online count did not increase for the BSP");

	cr_assert(cpu_set_state(bsp, CPU_STATE_ONLINE), "repeating ONLINE transition should still succeed");
	cr_assert_eq(cpu_online_count(), 1u, "repeating ONLINE transition double-counted the BSP");

	cr_assert(cpu_set_state(ap, CPU_STATE_ONLINE), "cpu_set_state failed for AP");
	cr_assert_eq(cpu_online_count(), 2u, "online count did not increase for the AP");

	cr_assert(cpu_set_state(bsp, CPU_STATE_PARKED), "cpu_set_state failed for BSP park");
	cr_assert_eq(cpu_online_count(), 1u, "online count did not decrease when BSP left ONLINE");

	cr_assert(cpu_set_state(bsp, CPU_STATE_PARKED), "repeating non-ONLINE transition should succeed");
	cr_assert_eq(cpu_online_count(), 1u, "repeating non-ONLINE transition changed online count");

	cr_assert(cpu_set_state(ap, CPU_STATE_HALTED), "cpu_set_state failed for AP halt");
	cr_assert_eq(cpu_online_count(), 0u, "online count did not drop back to zero");
	cr_assert(!cpu_set_state(NULL, CPU_STATE_ONLINE), "cpu_set_state accepted NULL");
	cr_assert_eq(cpu_state_get(NULL), CPU_STATE_HALTED, "cpu_state_get(NULL) should report HALTED");
}

Test(cpu_irq, exception_nesting_and_irq_restore_follow_current_cpu_and_saved_state) {
	struct irq_state saved_disabled;
	struct irq_state saved_enabled;

	init_bound_bootstrap_cpu();

	cr_assert(!irq_in_exception(), "CPU should not start in an exception");
	cpu_enter_exception();
	cpu_enter_exception();
	cr_assert(irq_in_exception(), "nested exception entry was not tracked");
	cr_assert_eq(cpu_current()->exception_depth, 2u, "exception depth mismatch after nested entry");

	cpu_leave_exception();
	cr_assert(irq_in_exception(), "CPU should still be in an exception after one leave");
	cpu_leave_exception();
	cpu_leave_exception();
	cr_assert(!irq_in_exception(), "CPU should have left exception context");
	cr_assert_eq(cpu_current()->exception_depth, 0u, "exception depth underflowed");

	irq_disable_local();
	saved_disabled = irq_save_disable();
	cr_assert(!saved_disabled.enabled, "irq_save_disable should report previously disabled interrupts");
	cr_assert(!irq_enabled(), "irq_save_disable unexpectedly re-enabled interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "irq depth mismatch after disabled save");
	irq_restore(saved_disabled);
	cr_assert(!irq_enabled(), "irq_restore should preserve a previously disabled state");
	cr_assert_eq(cpu_current()->irq_disable_depth, 0u, "irq depth mismatch after restoring disabled state");

	irq_enable_local();
	saved_enabled = irq_save_disable();
	cr_assert(saved_enabled.enabled, "irq_save_disable should report previously enabled interrupts");
	cr_assert(!irq_enabled(), "irq_save_disable did not disable interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "irq depth mismatch after enabled save");
	irq_restore(saved_enabled);
	cr_assert(irq_enabled(), "irq_restore did not re-enable interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 0u, "irq depth mismatch after restoring enabled state");

	reset_test_state();
}

Test(cpu_irq, irq_save_disable_is_safe_without_a_bound_cpu) {
	struct irq_state saved;

	reset_test_state();
	irq_enable_local();

	saved = irq_save_disable();
	cr_assert(saved.enabled, "irq_save_disable should still snapshot interrupt state without a CPU binding");
	cr_assert(!irq_enabled(), "irq_save_disable did not disable interrupts without a CPU binding");

	irq_restore(saved);
	cr_assert(irq_enabled(), "irq_restore did not restore interrupt state without a CPU binding");
}
