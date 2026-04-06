#include <core/cpu.h>
#include <core/spinlock.h>
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

Test(cpu, topology_initializes_from_generic_descriptors) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 10u,
         .arch_id         = 0x20u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x210000u,
         .boot_stack_top  = 0x214000u,
         .limine_mp_info  = (void*)0x1000u,
		 },
		{
         .index           = 1u,
         .processor_id    = 11u,
         .arch_id         = 0x21u,
         .role            = CPU_ROLE_BSP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
         .limine_mp_info  = (void*)0x2000u,
		 },
		{
         .index           = 2u,
         .processor_id    = 12u,
         .arch_id         = 0x22u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x220000u,
         .boot_stack_top  = 0x224000u,
         .limine_mp_info  = (void*)0x3000u,
		 },
	};
	struct cpu_topology* topology;
	struct cpu*          bsp;
	struct cpu*          ap0;
	struct cpu*          ap2;

	cr_assert(cpu_topology_init(init_info, 3u, 1u), "cpu_topology_init failed");

	topology = cpu_topology_get();
	bsp      = cpu_bsp();
	ap0      = cpu_by_index(0u);
	ap2      = cpu_by_index(2u);

	cr_assert_not_null(topology, "cpu_topology_get returned NULL");
	cr_assert_eq(cpu_count(), 3u, "cpu_count did not match the generic topology input");
	cr_assert_eq(cpu_online_count(), 0u, "online CPUs should be zero before boot publication");
	cr_assert_eq(topology->bsp_index, 1u, "BSP index did not match the generic topology input");
	cr_assert_not_null(bsp, "cpu_bsp returned NULL");
	cr_assert_eq(bsp->index, 1u, "cpu_bsp selected the wrong CPU");
	cr_assert_eq(bsp->arch_id, 0x21u, "cpu_bsp arch id mismatch");
	cr_assert_eq(bsp->processor_id, 11u, "cpu_bsp processor id mismatch");
	cr_assert_eq(bsp->boot_stack_base, 0x200000u, "BSP boot stack base mismatch");
	cr_assert_eq(bsp->boot_stack_top, 0x204000u, "BSP boot stack top mismatch");
	cr_assert_eq(bsp->limine_mp_info, (void*)0x2000u, "BSP opaque boot pointer mismatch");
	cr_assert_not_null(ap0, "first AP lookup failed");
	cr_assert_not_null(ap2, "second AP lookup failed");
	cr_assert_eq(ap0->role, CPU_ROLE_AP, "CPU0 should be marked as an AP");
	cr_assert_eq(ap2->role, CPU_ROLE_AP, "CPU2 should be marked as an AP");
	cr_assert_eq(ap0->boot_stack_top - ap0->boot_stack_base, 0x4000u, "AP0 boot stack size mismatch");
	cr_assert_eq(ap2->boot_stack_top - ap2->boot_stack_base, 0x4000u, "AP2 boot stack size mismatch");

	reset_test_state();
}

Test(cpu, irq_save_disable_and_restore_track_depth) {
	struct irq_state outer;
	struct irq_state inner;

	init_bound_bootstrap_cpu();

	cr_assert(irq_enabled(), "interrupts should start enabled in hosted tests");

	outer = irq_save_disable();
	cr_assert(!irq_enabled(), "irq_save_disable did not disable interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "outer irq_save_disable depth mismatch");

	inner = irq_save_disable();
	cr_assert(!irq_enabled(), "nested irq_save_disable unexpectedly re-enabled interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 2u, "nested irq_save_disable depth mismatch");

	irq_restore(inner);
	cr_assert(!irq_enabled(), "restoring nested disabled state should keep interrupts disabled");
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "nested irq_restore depth mismatch");

	irq_restore(outer);
	cr_assert(irq_enabled(), "restoring the outer state should re-enable interrupts");
	cr_assert_eq(cpu_current()->irq_disable_depth, 0u, "final irq_restore depth mismatch");

	reset_test_state();
}

Test(spinlock, debug_checks_reject_wrong_lock_order_and_require_irqsave) {
	struct spinlock  paging  = SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  vmm     = SPINLOCK_INIT_CLASS("vmm_lock", SPINLOCK_ORDER_VMM, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  irqsave = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
	struct irq_state state;

	init_bound_bootstrap_cpu();
	cpu_interrupts_set_ready(cpu_current(), true);

	cr_assert_eq(spinlock_debug_check_acquire(&irqsave),
	             SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED,
	             "IRQSAVE lock should reject acquisition while interrupts are enabled");

	state = spinlock_lock_irqsave(&paging);
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "spinlock_lock_irqsave did not disable interrupts");
	cr_assert_eq(spinlock_debug_check_acquire(&vmm),
	             SPINLOCK_DEBUG_CHECK_ORDER,
	             "lower-order lock should be rejected while a higher-order lock is held");
	spinlock_unlock_irqrestore(&paging, state);

	cr_assert(irq_enabled(), "spinlock_unlock_irqrestore did not restore interrupt state");
	cr_assert_eq(cpu_current()->irq_disable_depth, 0u, "spinlock_unlock_irqrestore left IRQ depth elevated");

	reset_test_state();
}
