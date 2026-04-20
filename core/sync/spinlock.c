#include <core/cpu.h>
#include <core/spinlock.h>

void spinlock_init(struct spinlock* lock) {
	if (!lock) return;

	lock->state = 0u;
#if CORE_LOCK_DEBUG
	lock->name      = NULL;
	lock->order     = SPINLOCK_ORDER_NONE;
	lock->flags     = SPINLOCK_FLAG_NONE;
	lock->owner_cpu = 0u;
#endif
}

void spinlock_init_class(struct spinlock* lock, const char* name, uint32_t order, uint32_t flags) {
	if (!lock) return;

	spinlock_init(lock);
#if CORE_LOCK_DEBUG
	lock->name  = name;
	lock->order = order;
	lock->flags = flags;
#else
	(void)name;
	(void)order;
	(void)flags;
#endif
}

void spinlock_relax(void) {
#if defined(PLATFORM_PC_X86_64)
	__asm__ volatile("pause");
#elif defined(PLATFORM_PC_AARCH64)
	__asm__ volatile("yield");
#else
	__asm__ volatile("" ::: "memory");
#endif
}

#if CORE_LOCK_DEBUG
static void spinlock_debug_record_acquire(struct spinlock* lock) {
	struct cpu* cpu = cpu_current();

	if (!lock || !cpu) return;
	if (cpu->lock_depth < CPU_DEBUG_LOCK_DEPTH) {
		cpu->lock_order_stack[cpu->lock_depth] = lock->order;
		cpu->lock_depth++;
	}
	lock->owner_cpu = (uint32_t)(cpu->index + 1u);
}

static void spinlock_debug_record_release(struct spinlock* lock) {
	struct cpu* cpu = cpu_current();

	if (!lock || !cpu) return;
	if (cpu->lock_depth != 0u) cpu->lock_depth--;
	lock->owner_cpu = 0u;
}

static void spinlock_debug_fail(enum spinlock_debug_check check) {
	(void)check;
	__builtin_trap();
}
#else
static void spinlock_debug_record_acquire(struct spinlock* lock) {
	(void)lock;
}

static void spinlock_debug_record_release(struct spinlock* lock) {
	(void)lock;
}

static void spinlock_debug_fail(enum spinlock_debug_check check) {
	(void)check;
}
#endif

enum spinlock_debug_check spinlock_debug_check_acquire(const struct spinlock* lock) {
#if CORE_LOCK_DEBUG
	const struct cpu* cpu;
	uint32_t          last_order;

	if (!lock) return SPINLOCK_DEBUG_CHECK_OK;

	cpu = cpu_current();
	if (!cpu) return SPINLOCK_DEBUG_CHECK_OK;
	if (lock->owner_cpu == cpu->index + 1u) return SPINLOCK_DEBUG_CHECK_REENTRANT;
	if ((lock->flags & SPINLOCK_FLAG_IRQSAVE) != 0 && cpu->interrupts_ready && irq_enabled()) {
		return SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED;
	}
	if ((lock->flags & SPINLOCK_FLAG_ALLOW_EXCEPTION) == 0 && irq_in_exception()) {
		return SPINLOCK_DEBUG_CHECK_EXCEPTION_DISALLOWED;
	}
	if (cpu->lock_depth == 0u) return SPINLOCK_DEBUG_CHECK_OK;

	last_order = cpu->lock_order_stack[cpu->lock_depth - 1u];
	if (lock->order != SPINLOCK_ORDER_NONE && last_order != SPINLOCK_ORDER_NONE && lock->order < last_order) {
		return SPINLOCK_DEBUG_CHECK_ORDER;
	}
#else
	(void)lock;
#endif
	return SPINLOCK_DEBUG_CHECK_OK;
}

bool spinlock_try_lock(struct spinlock* lock) {
	enum spinlock_debug_check check;

	check = spinlock_debug_check_acquire(lock);
	if (check != SPINLOCK_DEBUG_CHECK_OK) {
		spinlock_debug_fail(check);
		return false;
	}
	if (__atomic_exchange_n(&lock->state, 1u, __ATOMIC_ACQUIRE) != 0u) return false;

	spinlock_debug_record_acquire(lock);
	return true;
}

void spinlock_lock(struct spinlock* lock) {
	while (!spinlock_try_lock(lock)) {
		while (__atomic_load_n(&lock->state, __ATOMIC_RELAXED) != 0u) {
			spinlock_relax();
		}
	}
}

void spinlock_unlock(struct spinlock* lock) {
	spinlock_debug_record_release(lock);
	__atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

struct irq_state spinlock_lock_irqsave(struct spinlock* lock) {
	struct irq_state state = irq_save_disable();

	spinlock_lock(lock);
	return state;
}

void spinlock_unlock_irqrestore(struct spinlock* lock, struct irq_state state) {
	spinlock_unlock(lock);
	irq_restore(state);
}
