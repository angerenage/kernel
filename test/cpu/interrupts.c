#include "test_support.h"

Test(cpu, irq_save_disable_and_restore_track_depth) {
	struct irq_state outer;
	struct irq_state inner;

	cpu_test_init_bound_bootstrap();

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

	cpu_test_reset();
}

Test(cpu_irq, exception_nesting_and_irq_restore_follow_current_cpu_and_saved_state) {
	struct irq_state saved_disabled;
	struct irq_state saved_enabled;

	cpu_test_init_bound_bootstrap();

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

	cpu_test_reset();
}

Test(cpu_irq, irq_save_disable_is_safe_without_a_bound_cpu) {
	struct irq_state saved;

	cpu_test_reset();
	irq_enable_local();

	saved = irq_save_disable();
	cr_assert(saved.enabled, "irq_save_disable should still snapshot interrupt state without a CPU binding");
	cr_assert(!irq_enabled(), "irq_save_disable did not disable interrupts without a CPU binding");

	irq_restore(saved);
	cr_assert(irq_enabled(), "irq_restore did not restore interrupt state without a CPU binding");
}
