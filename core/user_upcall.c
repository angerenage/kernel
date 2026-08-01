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
	state->stack_id    = VMM_ID_INVALID;
	state->phase       = USER_UPCALL_PHASE_IDLE;
	state->initialized = true;
}

void uthread_upcall_state_deinit(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL) return;
	state = &thread->upcall;
	if (!state->initialized) return;

	irq_state          = spinlock_lock_irqsave(&state->lock);
	state->stack_id    = VMM_ID_INVALID;
	state->stack_top   = 0u;
	state->phase       = USER_UPCALL_PHASE_IDLE;
	state->initialized = false;
	memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
	uthread_upcall_clear_pending(state);
	spinlock_unlock_irqrestore(&state->lock, irq_state);
}

enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_request* request) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	size_t                    tail;

	if (thread == NULL || request == NULL || request->entry == 0u || !thread->upcall.initialized) {
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
	if (state->count == USER_UPCALL_QUEUE_CAPACITY) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_QUEUE_FULL;
	}

	tail                 = (state->head + state->count) % USER_UPCALL_QUEUE_CAPACITY;
	state->pending[tail] = *request;
	state->count++;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

size_t uthread_upcall_purge(struct uthread* thread, enum user_upcall_origin origin, uintptr_t origin_token) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	size_t                    original_count;
	size_t                    retained = 0u;
	size_t                    purged   = 0u;

	if (thread == NULL || origin == USER_UPCALL_ORIGIN_NONE || origin_token == 0u || !thread->upcall.initialized) {
		return 0u;
	}

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->initialized) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return 0u;
	}

	original_count = state->count;
	for (size_t i = 0u; i < original_count; i++) {
		size_t                     source  = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;
		struct user_upcall_request request = state->pending[source];

		if (request.origin == origin && request.origin_token == origin_token) {
			purged++;
			continue;
		}

		{
			size_t destination = (state->head + retained) % USER_UPCALL_QUEUE_CAPACITY;

			if (destination != source) state->pending[destination] = request;
		}
		retained++;
	}

	for (size_t i = retained; i < original_count; i++) {
		size_t index = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;

		memset(&state->pending[index], 0, sizeof(state->pending[index]));
	}
	state->count = retained;
	if (retained == 0u) state->head = 0u;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return purged;
}

enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	struct user_upcall_state*  state;
	struct user_upcall_request request;
	struct irq_state           irq_state;

	if (thread == NULL || frame == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (uthread_upcall_thread_dying(thread)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_THREAD_DYING;
	}
	switch (state->phase) {
	case USER_UPCALL_PHASE_RESUME:
		state->phase = USER_UPCALL_PHASE_IDLE;
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_DEFERRED;
	case USER_UPCALL_PHASE_ACTIVE:
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_IDLE;
	case USER_UPCALL_PHASE_IDLE:
		break;
	default:
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}
	if (state->count == 0u) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_IDLE;
	}
	if (state->stack_id == VMM_ID_INVALID || state->stack_top == 0u) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}

	request = state->pending[state->head];
	if (!hal_userspace_context_save(&state->interrupted_context, frame)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}
	if (!hal_userspace_frame_redirect(frame,
	                                  request.entry,
	                                  state->stack_top,
	                                  request.args[0],
	                                  request.args[1],
	                                  request.args[2],
	                                  request.args[3],
	                                  request.args[4])) {
		memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}

	memset(&state->pending[state->head], 0, sizeof(state->pending[state->head]));
	state->head = (state->head + 1u) % USER_UPCALL_QUEUE_CAPACITY;
	state->count--;
	state->phase = USER_UPCALL_PHASE_ACTIVE;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL || frame == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;

	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (state->phase != USER_UPCALL_PHASE_ACTIVE) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_NOT_ACTIVE;
	}
	if (!hal_userspace_context_restore(frame, &state->interrupted_context)) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_CONTEXT_INVALID;
	}

	memset(&state->interrupted_context, 0, sizeof(state->interrupted_context));
	state->phase = USER_UPCALL_PHASE_RESUME;
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
	active    = state->phase == USER_UPCALL_PHASE_ACTIVE;
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return active;
}
