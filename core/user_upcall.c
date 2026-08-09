#include <core/thread.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <libc/string.h>
#include <stddef.h>
#include <stdint.h>

#define USER_UPCALL_INTERNAL_FORCE_RESERVED ((uint32_t)1u << 31)

static bool uthread_upcall_thread_dying(const struct uthread* thread) {
	return thread != NULL && __atomic_load_n(&thread->dying, __ATOMIC_ACQUIRE) != 0u;
}

static bool uthread_upcall_request_valid(const struct user_upcall_request* request) {
	if (request == NULL || request->entry == 0u) return false;
	return (request->flags & ~((uint32_t)USER_UPCALL_FLAG_NON_EVICTABLE | (uint32_t)USER_UPCALL_FLAG_COALESCIBLE)) ==
	       0u;
}

static void uthread_upcall_record_drop_locked(struct user_upcall_state* state) {
	if (state == NULL || state->dropped_count == UINT64_MAX) return;
	state->dropped_count++;
}

static bool uthread_upcall_request_non_evictable(const struct user_upcall_request* request) {
	return request != NULL && (request->flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u;
}

static bool uthread_upcall_request_coalescible(const struct user_upcall_request* request) {
	return request != NULL && (request->flags & USER_UPCALL_FLAG_COALESCIBLE) != 0u;
}

static bool uthread_upcall_request_force_reserved(const struct user_upcall_request* request) {
	return request != NULL && (request->flags & USER_UPCALL_INTERNAL_FORCE_RESERVED) != 0u;
}

static void uthread_upcall_remove_offset_locked(struct user_upcall_state* state, size_t offset) {
	if (state == NULL || offset >= state->count) return;

	for (size_t i = offset; i + 1u < state->count; i++) {
		size_t destination = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;
		size_t source      = (state->head + i + 1u) % USER_UPCALL_QUEUE_CAPACITY;

		state->pending[destination] = state->pending[source];
	}
	{
		size_t tail = (state->head + state->count - 1u) % USER_UPCALL_QUEUE_CAPACITY;

		memset(&state->pending[tail], 0, sizeof(state->pending[tail]));
	}
	state->count--;
}

static void uthread_upcall_rebalance_force_reservations_locked(struct user_upcall_state* state) {
	if (state == NULL) return;

	while (state->force_eviction_reservations != 0u &&
	       state->count + state->force_free_reservations < USER_UPCALL_QUEUE_CAPACITY) {
		bool converted = false;

		for (size_t i = 0u; i < state->count; i++) {
			size_t index = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;

			if (!uthread_upcall_request_force_reserved(&state->pending[index])) continue;
			state->pending[index].flags &= ~USER_UPCALL_INTERNAL_FORCE_RESERVED;
			state->force_eviction_reservations--;
			state->force_free_reservations++;
			converted = true;
			break;
		}
		if (!converted) break;
	}
}

static void uthread_upcall_clear_pending(struct user_upcall_state* state) {
	state->head                        = 0u;
	state->count                       = 0u;
	state->force_free_reservations     = 0u;
	state->force_eviction_reservations = 0u;
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
	thread_clear_interrupt(&thread->thread);
	spinlock_unlock_irqrestore(&state->lock, irq_state);
}

static enum user_upcall_result uthread_upcall_enqueue_internal(struct uthread*                   thread,
                                                               const struct user_upcall_request* request,
                                                               bool                              coalesce_matching) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	size_t                    original_count;
	size_t                    retained;
	size_t                    tail;

	if (thread == NULL || !uthread_upcall_request_valid(request) || !thread->upcall.initialized) {
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

	original_count = state->count;
	retained       = 0u;
	if (coalesce_matching) {
		for (size_t i = 0u; i < original_count; i++) {
			size_t                     source  = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;
			struct user_upcall_request pending = state->pending[source];

			if (pending.origin == request->origin && pending.origin_token == request->origin_token &&
			    uthread_upcall_request_coalescible(&pending) && uthread_upcall_request_coalescible(request) &&
			    !uthread_upcall_request_non_evictable(&pending) && !uthread_upcall_request_force_reserved(&pending)) {
				continue;
			}
			{
				size_t destination = (state->head + retained) % USER_UPCALL_QUEUE_CAPACITY;

				if (destination != source) state->pending[destination] = pending;
			}
			retained++;
		}
		state->count = retained;
	}
	if (state->count + state->force_free_reservations >= USER_UPCALL_QUEUE_CAPACITY) {
		uthread_upcall_record_drop_locked(state);
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_QUEUE_FULL;
	}

	tail                 = (state->head + state->count) % USER_UPCALL_QUEUE_CAPACITY;
	state->pending[tail] = *request;
	state->count++;
	for (size_t i = state->count; i < original_count; i++) {
		size_t index = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;

		memset(&state->pending[index], 0, sizeof(state->pending[index]));
	}
	uthread_upcall_rebalance_force_reservations_locked(state);
	thread_request_interrupt(&thread->thread);
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_request* request) {
	return uthread_upcall_enqueue_internal(thread, request, false);
}

enum user_upcall_result uthread_upcall_enqueue_latest(struct uthread*                   thread,
                                                      const struct user_upcall_request* request) {
	if (request == NULL || request->origin == USER_UPCALL_ORIGIN_NONE || request->origin_token == 0u ||
	    !uthread_upcall_request_coalescible(request)) {
		return USER_UPCALL_INVALID_ARGUMENTS;
	}
	return uthread_upcall_enqueue_internal(thread, request, true);
}

enum user_upcall_result uthread_upcall_force_reserve(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL || !thread->upcall.initialized) return USER_UPCALL_INVALID_ARGUMENTS;
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
	if (state->count + state->force_free_reservations < USER_UPCALL_QUEUE_CAPACITY) {
		state->force_free_reservations++;
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_OK;
	}

	for (size_t i = 0u; i < state->count; i++) {
		size_t                      index   = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;
		struct user_upcall_request* pending = &state->pending[index];

		if (uthread_upcall_request_non_evictable(pending) || uthread_upcall_request_force_reserved(pending)) {
			continue;
		}
		pending->flags |= USER_UPCALL_INTERNAL_FORCE_RESERVED;
		state->force_eviction_reservations++;
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_OK;
	}

	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_QUEUE_FULL;
}

void uthread_upcall_force_cancel(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;

	if (thread == NULL || !thread->upcall.initialized) return;
	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->initialized) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return;
	}
	if (state->force_free_reservations != 0u) {
		state->force_free_reservations--;
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return;
	}
	for (size_t i = 0u; i < state->count; i++) {
		size_t index = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;

		if (!uthread_upcall_request_force_reserved(&state->pending[index])) continue;
		state->pending[index].flags &= ~USER_UPCALL_INTERNAL_FORCE_RESERVED;
		if (state->force_eviction_reservations != 0u) state->force_eviction_reservations--;
		break;
	}
	spinlock_unlock_irqrestore(&state->lock, irq_state);
}

enum user_upcall_result uthread_upcall_force_commit(struct uthread* thread, const struct user_upcall_request* request) {
	struct user_upcall_request committed;
	struct user_upcall_state*  state;
	struct irq_state           irq_state;
	size_t                     tail;

	if (thread == NULL || !uthread_upcall_request_valid(request) || !thread->upcall.initialized) {
		return USER_UPCALL_INVALID_ARGUMENTS;
	}

	state     = &thread->upcall;
	committed = *request;
	committed.flags &= ~USER_UPCALL_INTERNAL_FORCE_RESERVED;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (!state->initialized) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_INVALID_ARGUMENTS;
	}

	if (state->force_free_reservations != 0u) {
		if (state->count >= USER_UPCALL_QUEUE_CAPACITY) {
			spinlock_unlock_irqrestore(&state->lock, irq_state);
			return USER_UPCALL_INVALID_ARGUMENTS;
		}
		state->force_free_reservations--;
	}
	else {
		size_t victim = SIZE_MAX;

		for (size_t i = 0u; i < state->count; i++) {
			size_t index = (state->head + i) % USER_UPCALL_QUEUE_CAPACITY;

			if (!uthread_upcall_request_force_reserved(&state->pending[index])) continue;
			victim = i;
			break;
		}
		if (victim == SIZE_MAX) {
			spinlock_unlock_irqrestore(&state->lock, irq_state);
			return USER_UPCALL_INVALID_ARGUMENTS;
		}
		uthread_upcall_remove_offset_locked(state, victim);
		if (state->force_eviction_reservations != 0u) state->force_eviction_reservations--;
		uthread_upcall_record_drop_locked(state);
	}

	tail                 = (state->head + state->count) % USER_UPCALL_QUEUE_CAPACITY;
	state->pending[tail] = committed;
	state->count++;
	thread_request_interrupt(&thread->thread);
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return USER_UPCALL_OK;
}

enum user_upcall_result uthread_upcall_enqueue_force(struct uthread*                   thread,
                                                     const struct user_upcall_request* request) {
	enum user_upcall_result result;

	if (thread == NULL || !uthread_upcall_request_valid(request) || !thread->upcall.initialized) {
		return USER_UPCALL_INVALID_ARGUMENTS;
	}
	result = uthread_upcall_force_reserve(thread);
	if (result != USER_UPCALL_OK) {
		if (result == USER_UPCALL_QUEUE_FULL && thread != NULL && thread->upcall.initialized) {
			struct irq_state irq_state = spinlock_lock_irqsave(&thread->upcall.lock);

			if (thread->upcall.initialized) uthread_upcall_record_drop_locked(&thread->upcall);
			spinlock_unlock_irqrestore(&thread->upcall.lock, irq_state);
		}
		return result;
	}
	result = uthread_upcall_force_commit(thread, request);
	if (result != USER_UPCALL_OK) uthread_upcall_force_cancel(thread);
	return result;
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
			if (uthread_upcall_request_force_reserved(&request) && state->force_eviction_reservations != 0u) {
				state->force_eviction_reservations--;
				state->force_free_reservations++;
			}
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
	uthread_upcall_rebalance_force_reservations_locked(state);
	if (retained == 0u) thread_clear_interrupt(&thread->thread);
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
	if (uthread_upcall_request_force_reserved(&state->pending[state->head])) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return USER_UPCALL_DEFERRED;
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
	uthread_upcall_rebalance_force_reservations_locked(state);
	if (state->count == 0u) thread_clear_interrupt(&thread->thread);
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

uint64_t uthread_upcall_dropped_count(struct uthread* thread) {
	struct user_upcall_state* state;
	struct irq_state          irq_state;
	uint64_t                  count;

	if (thread == NULL || !thread->upcall.initialized) return 0u;
	state     = &thread->upcall;
	irq_state = spinlock_lock_irqsave(&state->lock);
	count     = state->dropped_count;
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
