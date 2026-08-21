#include "test_support.h"

Test(spinlock, debug_checks_reject_wrong_lock_order_and_require_irqsave) {
	struct spinlock paging = SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock address_space =
		SPINLOCK_INIT_CLASS("address_space_lock", SPINLOCK_ORDER_VADDR, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  irqsave = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
	struct irq_state state;

	cpu_test_init_bound_bootstrap();
	cpu_interrupts_set_ready(cpu_current(), true);

	cr_assert_eq(spinlock_debug_check_acquire(&irqsave),
	             SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED,
	             "IRQSAVE lock should reject acquisition while interrupts are enabled");

	state = spinlock_lock_irqsave(&paging);
	cr_assert_eq(cpu_current()->irq_disable_depth, 1u, "spinlock_lock_irqsave did not disable interrupts");
	cr_assert_eq(spinlock_debug_check_acquire(&address_space),
	             SPINLOCK_DEBUG_CHECK_ORDER,
	             "lower-order lock should be rejected while a higher-order lock is held");
	spinlock_unlock_irqrestore(&paging, state);

	cr_assert(irq_enabled(), "spinlock_unlock_irqrestore did not restore interrupt state");
	cr_assert_eq(cpu_current()->irq_disable_depth, 0u, "spinlock_unlock_irqrestore left IRQ depth elevated");

	cpu_test_reset();
}
