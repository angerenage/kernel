#include <core/user_upcall.h>
#include <core/uthread.h>
#include <libc/string.h>
#include <stddef.h>
#include <stdint.h>

static bool uthread_upcall_thread_dying(const struct uthread* thread) {
	return thread != NULL && __atomic_load_n(&thread->dying, __ATOMIC_ACQUIRE) != 0u;
}

static void uthread_upcall_clear_pending(struct user_upcall_state* state) {
	state->head  = 0u;
	state->count = 0u;
	memset(state->pending, 0, sizeof(state->pending));
}

void uthread_upcall_state_init(struct uthread* thread) {
	struct user_upcall_state* state;

	if (thread == NULL) return;

	state = &thread->upcall;
	memset(state, 0, sizeof(*state));
	spinlock_init_class(&state->lock,
	                    "uthread_upcall",
	                    SPINLOCK_ORDER_USER_UPCALL,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	state->initialized = true;
}

void uthread_upcall_state_deinit(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL) return;
	state = &thread->upcall;
	if (!state->initialized) return;

	irq_state          = spinlock_lock_irqsave(&state->lock);
	state->entry       = 0u;
	state->stack_top   = 0u;
	state->configured  = false;
	state->active      = false;
	state->initialized = false;
	memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
	uthread_upcall_clear_pending(state);
	spinlock_unlock_irqrestore(&state->lock, irq_state);
}

enum user_upcall_result uthread_upcall_configure(struct uthread* thread, uintptr_t entry, uintptr_t stack_top) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	bool                      disable;

	if (thread == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;
	disable = entry == 0u && stack_top == 0u;
	if (!disable && (entry == 0u || stack_top == 0u)) return USER_UPCALL_INVALID_ARGUMENTS;
	if (!disable && (stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) {
		return USER_UPCALL_INVALID_ARGUMENTS;
	}
	if (uthread_upcall_thread_dying(thread)) return USER_UPCALL_THREAD_DYING;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->initialized) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_INVALID_ARGUMENTS;
	}
	if (uthread_upcall_thread_dying(thread)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_THREAD_DYING;
	}
	if (state->active) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_BUSY;
	}

	state->entry      = entry;
	state->stack_top  = stack_top;
	state->configured = !disable;
	if (disable) {
		memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
		uthread_upcall_clear_pending(state);
	}
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_event* event) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	size_t                    tail;

	if (thread == NULL || event == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;
	if (uthread_upcall_thread_dying(thread)) return USER_UPCALL_THREAD_DYING;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->configured) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_NOT_CONFIGURED;
	}
	if (uthread_upcall_thread_dying(thread)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_THREAD_DYING;
	}
	if (state->count == USER_UPCALL_QUEUE_CAPACITY) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_QUEUE_FULL;
	}

	tail                 = (state->head + state->count) % USER_UPCALL_QUEUE_CAPACITY;
	state->pending[tail] = *event;
	state->count++;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	struct user_upcall_state* state;
	struct user_upcall_event  event;
	struct irq_state          irq_state;

	if (thread == NULL || frame == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (uthread_upcall_thread_dying(thread)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_THREAD_DYING;
	}
	if (!state->configured || state->active || state->count == 0u) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_IDLE;
	}

	event = state->pending[state->head];
	if (!hal_userspace_context_save(&state->interrupted_context, frame)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}
	if (!hal_userspace_frame_redirect(
			frame, state->entry, state->stack_top, event.args[0], event.args[1], event.args[2])) {
		memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}

	memset(&state->pending[state->head], 0, sizeof(state->pending[state->head]));
	state->head = (state->head + 1u) % USER_UPCALL_QUEUE_CAPACITY;
	state->count--;
	state->active = true;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL || frame == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->active) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_NOT_ACTIVE;
	}
	if (!hal_userspace_context_restore(frame, &state->interrupted_context)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}

	memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
	state->active = false;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

size_t uthread_upcall_pending_count(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	size_t                    count;

	if (thread == NULL || !thread->upcall.initialized) return 0u;
	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	count     = state->count;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return count;
}

bool uthread_upcall_is_active(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	bool                      active;

	if (thread == NULL || !thread->upcall.initialized) return false;
	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	active    = state->active;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return active;
}
