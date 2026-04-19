#include <core/cpu.h>
#include <core/spinlock.h>
#include <hal/interrupts.h>
#include <kernel/cpu_boot.h>

#include "../selftest.h"

static void kernel_selftest_cpu_topology_is_consistent(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();
	struct cpu*          bsp      = cpu_bsp();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");
	KERNEL_SELFTEST_ASSERT(ctx, topology->cpu_count > 0u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_count() == topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() >= 1u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() <= cpu_count());
	KERNEL_SELFTEST_ASSERT_MSG(ctx, bsp != NULL, "cpu_bsp returned NULL");
	KERNEL_SELFTEST_ASSERT(ctx, topology->bsp_index < topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, bsp == &topology->cpus[topology->bsp_index]);
	KERNEL_SELFTEST_ASSERT(ctx, bsp->role == CPU_ROLE_BSP);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_state_get(bsp) == CPU_STATE_ONLINE);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_current() == bsp);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_is_bsp());
	KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(bsp));
	if (cpu_count() > 1u) {
		KERNEL_SELFTEST_ASSERT_MSG(ctx, cpu_online_count() == cpu_count(), "not all discovered CPUs reached ONLINE");
	}
}

static void kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");

	for (size_t i = 0; i < topology->cpu_count; i++) {
		const struct cpu* cpu = &topology->cpus[i];

		KERNEL_SELFTEST_ASSERT(ctx, cpu->index == i);
		KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(cpu));

		for (size_t j = i + 1; j < topology->cpu_count; j++) {
			KERNEL_SELFTEST_ASSERT_MSG(
				ctx, cpu->arch_id != topology->cpus[j].arch_id, "duplicate cpu arch_id discovered during boot");
		}
	}
}

static void kernel_selftest_cpu_current_accessors_match_bound_cpu(struct kernel_selftest_context* ctx) {
	struct cpu* current = cpu_current();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, current != NULL, "cpu_current returned NULL");
	KERNEL_SELFTEST_ASSERT(ctx, cpu_index() == current->index);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_arch_id() == current->arch_id);
	KERNEL_SELFTEST_ASSERT(ctx, current->boot_stack_top > current->boot_stack_base);
	KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, current->exception_depth == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, !cpu_irq_in_exception());
	KERNEL_SELFTEST_ASSERT(ctx, current->interrupts_ready);
}

static void kernel_selftest_cpu_irq_save_disable_tracks_nesting(struct kernel_selftest_context* ctx) {
	struct cpu*      current = cpu_current();
	struct irq_state outer;
	struct irq_state inner;
	bool             irq_was_enabled;
	bool             outer_saved = false;
	bool             inner_saved = false;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, current != NULL, "cpu_current returned NULL", cleanup);
	irq_was_enabled = irq_enabled();
	if (!irq_was_enabled) irq_enable_local();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 0u, cleanup);

	outer       = irq_save_disable();
	outer_saved = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, outer.enabled, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);

	inner       = irq_save_disable();
	inner_saved = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, inner.enabled == false, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 2u, cleanup);

	irq_restore(inner);
	inner_saved = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);

	irq_restore(outer);
	outer_saved = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 0u, cleanup);

cleanup:
	if (inner_saved) irq_restore(inner);
	if (outer_saved) irq_restore(outer);
	if (current != NULL && current->irq_disable_depth == 0u) {
		if (irq_was_enabled) irq_enable_local();
		else irq_disable_local();
	}
	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, irq_enabled() == irq_was_enabled);
		KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	}
}

static void kernel_selftest_cpu_spinlock_debug_checks_enforce_irqsave_and_order(struct kernel_selftest_context* ctx) {
	struct spinlock  paging     = SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  vmm        = SPINLOCK_INIT_CLASS("vmm_lock", SPINLOCK_ORDER_VMM, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  irqsave    = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
	struct cpu*      current    = cpu_current();
	struct irq_state state      = {0};
	bool             locked     = false;
	bool             ready_prev = false;
	bool             irq_was_enabled;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, current != NULL, "cpu_current returned NULL", cleanup);
	ready_prev      = current->interrupts_ready;
	irq_was_enabled = irq_enabled();
	cpu_interrupts_set_ready(current, true);
	if (!irq_was_enabled) irq_enable_local();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, spinlock_debug_check_acquire(&irqsave) == SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED, cleanup);

	state  = spinlock_lock_irqsave(&paging);
	locked = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, spinlock_debug_check_acquire(&vmm) == SPINLOCK_DEBUG_CHECK_ORDER, cleanup);

cleanup:
	if (locked) spinlock_unlock_irqrestore(&paging, state);
	if (current != NULL && current->irq_disable_depth == 0u) {
		if (irq_was_enabled) irq_enable_local();
		else irq_disable_local();
	}
	if (current != NULL) cpu_interrupts_set_ready(current, ready_prev);
	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, irq_enabled() == irq_was_enabled);
		KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	}
}

static const struct kernel_selftest_case kernel_cpu_selftests[] = {
	{
     .name = "topology_is_consistent",
     .run  = kernel_selftest_cpu_topology_is_consistent,
	 },
	{
     .name = "ids_are_unique_and_bindings_succeeded",
     .run  = kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded,
	 },
	{
     .name = "current_accessors_match_bound_cpu",
     .run  = kernel_selftest_cpu_current_accessors_match_bound_cpu,
	 },
	{
     .name = "irq_save_disable_tracks_nesting",
     .run  = kernel_selftest_cpu_irq_save_disable_tracks_nesting,
	 },
	{
     .name = "spinlock_debug_checks_enforce_irqsave_and_order",
     .run  = kernel_selftest_cpu_spinlock_debug_checks_enforce_irqsave_and_order,
	 },
};

const struct kernel_selftest_suite kernel_cpu_selftest_suite = {
	.name       = "cpu",
	.cases      = kernel_cpu_selftests,
	.case_count = sizeof(kernel_cpu_selftests) / sizeof(kernel_cpu_selftests[0]),
};
