#include <base/signal.h>
#include <base/upcall.h>
#include <core/id_table.h>
#include <core/sched.h>
#include <core/signal.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <hal/hcf.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(uintptr_t) >= sizeof(uint64_t), "signal payload requires 64-bit userspace words");

struct signal_handler_binding {
	struct signal_handler_binding* next;
	struct uthread*                target;
	uintptr_t                      entry;
};

struct signal_wait_binding {
	struct signal_wait_binding* next;
	struct uthread*             target;
	struct signal_payload       pending;
	uint64_t                    last_seen_generation;
	uint64_t                    pending_generation;
	bool                        pending_delivery;
	bool                        waiting;
};

static struct id_table signal_table = {
	.lock    = SPINLOCK_INIT_CLASS("signal_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = 1u,
	.min_id  = 1u,
	.max_id  = UINT64_MAX,
};

static bool signal_retain_callback(void* value, void* context) {
	(void)context;
	return signal_retain((struct signal*)value);
}

static uint64_t signal_next_generation(uint64_t generation) {
	return generation == UINT64_MAX ? 1u : generation + 1u;
}

static struct signal_handler_binding* signal_find_handler_locked(struct signal* signal, struct uthread* target,
                                                                 struct signal_handler_binding** out_previous) {
	struct signal_handler_binding* previous = NULL;
	struct signal_handler_binding* binding;

	if (out_previous != NULL) *out_previous = NULL;
	for (binding = signal->handler_head; binding != NULL; binding = binding->next) {
		if (binding->target == target) {
			if (out_previous != NULL) *out_previous = previous;
			return binding;
		}
		previous = binding;
	}
	return NULL;
}

static struct signal_wait_binding* signal_find_wait_locked(struct signal* signal, struct uthread* target,
                                                           struct signal_wait_binding** out_previous) {
	struct signal_wait_binding* previous = NULL;
	struct signal_wait_binding* binding;

	if (out_previous != NULL) *out_previous = NULL;
	for (binding = signal->wait_head; binding != NULL; binding = binding->next) {
		if (binding->target == target) {
			if (out_previous != NULL) *out_previous = previous;
			return binding;
		}
		previous = binding;
	}
	return NULL;
}

static struct signal_handler_binding* signal_remove_handler_locked(struct signal*                 signal,
                                                                   struct signal_handler_binding* previous,
                                                                   struct signal_handler_binding* binding) {
	if (signal == NULL || binding == NULL) return NULL;
	if (previous == NULL) {
		signal->handler_head = binding->next;
	}
	else {
		previous->next = binding->next;
	}
	if (signal->handler_tail == binding) signal->handler_tail = previous;
	if (signal->handler_count != 0u) signal->handler_count--;
	if (signal->handler_count == 0u) {
		signal->handler_head = NULL;
		signal->handler_tail = NULL;
	}
	binding->next = NULL;
	return binding;
}

static struct signal_wait_binding* signal_remove_wait_locked(struct signal*              signal,
                                                             struct signal_wait_binding* previous,
                                                             struct signal_wait_binding* binding) {
	if (signal == NULL || binding == NULL) return NULL;
	if (previous == NULL) {
		signal->wait_head = binding->next;
	}
	else {
		previous->next = binding->next;
	}
	if (signal->wait_tail == binding) signal->wait_tail = previous;
	if (signal->wait_receiver_count != 0u) signal->wait_receiver_count--;
	if (signal->wait_receiver_count == 0u) {
		signal->wait_head = NULL;
		signal->wait_tail = NULL;
	}
	binding->next    = NULL;
	binding->waiting = false;
	return binding;
}

static struct signal_handler_binding* signal_detach_handlers_locked(struct signal* signal) {
	struct signal_handler_binding* bindings = signal->handler_head;

	signal->handler_head  = NULL;
	signal->handler_tail  = NULL;
	signal->handler_count = 0u;
	return bindings;
}

static struct signal_wait_binding* signal_detach_waits_locked(struct signal* signal) {
	struct signal_wait_binding* bindings = signal->wait_head;

	signal->wait_head           = NULL;
	signal->wait_tail           = NULL;
	signal->wait_receiver_count = 0u;
	return bindings;
}

static void signal_release_handlers(struct signal_handler_binding* binding) {
	while (binding != NULL) {
		struct signal_handler_binding* next = binding->next;

		(void)uthread_upcall_purge(binding->target, USER_UPCALL_ORIGIN_SIGNAL, (uintptr_t)binding);
		uthread_release(binding->target);
		free(binding);
		binding = next;
	}
}

static void signal_release_waits(struct signal_wait_binding* binding) {
	while (binding != NULL) {
		struct signal_wait_binding* next = binding->next;

		uthread_release(binding->target);
		free(binding);
		binding = next;
	}
}

static enum signal_result signal_prepare_wait_receiver(struct signal* signal, struct uthread* target) {
	struct signal_wait_binding* candidate;
	struct signal_wait_binding* existing;
	struct irq_state            state;
	bool                        keep_candidate = false;
	enum signal_result          result         = SIGNAL_OK;

	if (signal == NULL || target == NULL) return SIGNAL_INVALID_ARGUMENTS;

	state    = spinlock_lock_irqsave(&signal->lock);
	existing = signal_find_wait_locked(signal, target, NULL);
	if (signal->closing) result = SIGNAL_CLOSED;
	spinlock_unlock_irqrestore(&signal->lock, state);
	if (result != SIGNAL_OK || existing != NULL) return result;

	if (!uthread_retain(target)) return SIGNAL_UNAVAILABLE;
	candidate = malloc(sizeof(*candidate));
	if (candidate == NULL) {
		uthread_release(target);
		return SIGNAL_NO_MEMORY;
	}
	*candidate = (struct signal_wait_binding){
		.target = target,
	};

	state    = spinlock_lock_irqsave(&signal->lock);
	existing = signal_find_wait_locked(signal, target, NULL);
	if (signal->closing) {
		result = SIGNAL_CLOSED;
	}
	else if (__atomic_load_n(&target->dying, __ATOMIC_ACQUIRE) != 0u) {
		result = SIGNAL_UNAVAILABLE;
	}
	else if (existing == NULL) {
		if (signal->wait_tail == NULL) {
			signal->wait_head = candidate;
		}
		else {
			signal->wait_tail->next = candidate;
		}
		signal->wait_tail = candidate;
		signal->wait_receiver_count++;
		keep_candidate = true;
	}
	spinlock_unlock_irqrestore(&signal->lock, state);

	if (!keep_candidate) {
		free(candidate);
		uthread_release(target);
	}
	return result;
}

static bool signal_take_wait_value_locked(struct signal* signal, struct signal_wait_binding* binding,
                                          struct signal_payload* out_payload) {
	if (binding->pending_delivery) {
		*out_payload                  = binding->pending;
		binding->last_seen_generation = binding->pending_generation;
		binding->pending              = (struct signal_payload){0};
		binding->pending_generation   = 0u;
		binding->pending_delivery     = false;
		binding->waiting              = false;
		return true;
	}
	if (!signal->has_value || signal->generation == binding->last_seen_generation) return false;

	*out_payload                  = signal->latest;
	binding->last_seen_generation = signal->generation;
	binding->waiting              = false;
	return true;
}

static void signal_clear_waiting(struct signal* signal, struct uthread* target) {
	struct signal_wait_binding* binding;
	struct irq_state            state;

	if (signal == NULL || target == NULL) return;
	state   = spinlock_lock_irqsave(&signal->lock);
	binding = signal_find_wait_locked(signal, target, NULL);
	if (binding != NULL) binding->waiting = false;
	spinlock_unlock_irqrestore(&signal->lock, state);
}

struct signal* signal_create(void) {
	struct signal*       signal;
	id_table_id_t        id;
	enum id_table_result id_result;

	signal = malloc(sizeof(*signal));
	if (signal == NULL) return NULL;
	memset(signal, 0, sizeof(*signal));
	spinlock_init_class(
		&signal->lock, "signal_lock", SPINLOCK_ORDER_SIGNAL, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	thread_wait_queue_init(&signal->waiters);
	spinlock_init_class(&signal->waiters.lock,
	                    "signal_waiters",
	                    SPINLOCK_ORDER_SCHED,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	signal->reference_count = 1u;

	id_result = id_table_alloc(&signal_table, signal, &id);
	if (id_result != ID_TABLE_OK) {
		free(signal);
		return NULL;
	}
	signal->id = (signal_id_t)id;
	return signal;
}

signal_id_t signal_id(const struct signal* signal) {
	return signal == NULL ? SIGNAL_ID_INVALID : signal->id;
}

bool signal_retain(struct signal* signal) {
	struct irq_state state;
	uint64_t         reference_count;
	bool             retained = false;

	if (signal == NULL) return false;

	state           = spinlock_lock_irqsave(&signal->lock);
	reference_count = __atomic_load_n(&signal->reference_count, __ATOMIC_ACQUIRE);
	if (!signal->closing && reference_count != 0u && reference_count != UINT64_MAX) {
		(void)__atomic_add_fetch(&signal->reference_count, 1u, __ATOMIC_ACQ_REL);
		retained = true;
	}
	spinlock_unlock_irqrestore(&signal->lock, state);
	return retained;
}

struct signal* signal_acquire(signal_id_t id) {
	if (id == SIGNAL_ID_INVALID) return NULL;
	return id_table_lookup_retain(&signal_table, (id_table_id_t)id, signal_retain_callback, NULL);
}

void signal_release(struct signal* signal) {
	if (signal == NULL) return;
	if (__atomic_sub_fetch(&signal->reference_count, 1u, __ATOMIC_ACQ_REL) != 0u) return;
	free(signal);
}

enum signal_result signal_destroy(struct signal* signal) {
	struct signal_handler_binding* handlers;
	struct signal_wait_binding*    waits;
	struct irq_state               state;
	struct signal*                 removed = NULL;
	enum id_table_result           id_result;
	signal_id_t                    id;

	if (signal == NULL) return SIGNAL_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&signal->lock);
	if (signal->closing) {
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_CLOSED;
	}
	id              = signal->id;
	signal->closing = true;
	spinlock_unlock_irqrestore(&signal->lock, state);

	id_result = id_table_remove(&signal_table, (id_table_id_t)id, (void**)&removed);
	if (id_result != ID_TABLE_OK) {
		state           = spinlock_lock_irqsave(&signal->lock);
		signal->closing = false;
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_NOT_FOUND;
	}
	if (removed != signal) hcf();

	state              = spinlock_lock_irqsave(&signal->lock);
	signal->id         = SIGNAL_ID_INVALID;
	handlers           = signal_detach_handlers_locked(signal);
	waits              = signal_detach_waits_locked(signal);
	signal->latest     = (struct signal_payload){0};
	signal->generation = 0u;
	signal->has_value  = false;
	spinlock_unlock_irqrestore(&signal->lock, state);

	(void)sched_wake_all(&signal->waiters);
	signal_release_handlers(handlers);
	signal_release_waits(waits);
	signal_release(signal);
	return SIGNAL_OK;
}

enum signal_result signal_send(struct signal* signal, process_id_t sender, const struct signal_payload* payload,
                               uint64_t* out_receiver_count, uint64_t* out_delivery_count) {
	struct signal_handler_binding* handler;
	struct signal_wait_binding*    wait;
	struct user_upcall_request     request;
	struct irq_state               state;
	uint64_t                       receiver_count         = 0u;
	uint64_t                       delivery_count         = 0u;
	size_t                         waiting_delivery_count = 0u;

	if (out_receiver_count != NULL) *out_receiver_count = 0u;
	if (out_delivery_count != NULL) *out_delivery_count = 0u;
	if (signal == NULL || payload == NULL) return SIGNAL_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&signal->lock);
	if (signal->closing) {
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_CLOSED;
	}

	signal->generation = signal_next_generation(signal->generation);
	signal->latest     = *payload;
	signal->has_value  = true;

	for (handler = signal->handler_head; handler != NULL; handler = handler->next) {
		enum user_upcall_result upcall_result;

		receiver_count++;
		if (__atomic_load_n(&handler->target->dying, __ATOMIC_ACQUIRE) != 0u) continue;
		request = (struct user_upcall_request){
			.origin       = USER_UPCALL_ORIGIN_SIGNAL,
			.origin_token = (uintptr_t)handler,
			.entry        = handler->entry,
			.args =
				{
					   (uintptr_t)sender,
					   (uintptr_t)payload->args[0],
					   (uintptr_t)payload->args[1],
					   (uintptr_t)payload->args[2],
					   (uintptr_t)payload->args[3],
					   },
		};
		upcall_result = uthread_upcall_enqueue(handler->target, &request);
		if (upcall_result == USER_UPCALL_OK) delivery_count++;
	}

	for (wait = signal->wait_head; wait != NULL; wait = wait->next) {
		if (!wait->waiting) continue;
		receiver_count++;
		if (__atomic_load_n(&wait->target->dying, __ATOMIC_ACQUIRE) != 0u) continue;

		wait->pending            = *payload;
		wait->pending_generation = signal->generation;
		wait->pending_delivery   = true;
		wait->waiting            = false;
		delivery_count++;
		waiting_delivery_count++;
	}
	spinlock_unlock_irqrestore(&signal->lock, state);

	if (waiting_delivery_count != 0u) (void)sched_wake_all(&signal->waiters);
	if (out_receiver_count != NULL) *out_receiver_count = receiver_count;
	if (out_delivery_count != NULL) *out_delivery_count = delivery_count;
	return SIGNAL_OK;
}

enum signal_result signal_read(struct signal* signal, struct signal_payload* out_payload) {
	struct irq_state state;

	if (signal == NULL || out_payload == NULL) return SIGNAL_INVALID_ARGUMENTS;
	state = spinlock_lock_irqsave(&signal->lock);
	if (signal->closing) {
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_CLOSED;
	}
	if (!signal->has_value) {
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_NO_VALUE;
	}
	*out_payload = signal->latest;
	spinlock_unlock_irqrestore(&signal->lock, state);
	return SIGNAL_OK;
}

enum signal_result signal_try_wait(struct signal* signal, struct signal_payload* out_payload) {
	struct signal_wait_binding* binding;
	struct uthread*             current;
	struct irq_state            state;
	enum signal_result          result;

	if (signal == NULL || out_payload == NULL) return SIGNAL_INVALID_ARGUMENTS;
	current = uthread_current();
	if (current == NULL || thread_is_terminated(&current->thread)) return SIGNAL_WAIT_FAILED;
	if (thread_should_cancel(&current->thread)) return SIGNAL_WAIT_CANCELED;

	result = signal_prepare_wait_receiver(signal, current);
	if (result != SIGNAL_OK) return result;

	state = spinlock_lock_irqsave(&signal->lock);
	if (signal->closing) {
		result = SIGNAL_CLOSED;
	}
	else if ((binding = signal_find_wait_locked(signal, current, NULL)) == NULL) {
		result = SIGNAL_WAIT_FAILED;
	}
	else if (signal_take_wait_value_locked(signal, binding, out_payload)) {
		result = SIGNAL_OK;
	}
	else {
		result = SIGNAL_WOULD_BLOCK;
	}
	spinlock_unlock_irqrestore(&signal->lock, state);
	return result;
}

enum signal_result signal_wait(struct signal* signal, struct signal_payload* out_payload) {
	struct signal_wait_binding* binding;
	struct uthread*             current;
	enum signal_result          result;

	if (signal == NULL || out_payload == NULL) return SIGNAL_INVALID_ARGUMENTS;
	current = uthread_current();
	if (current == NULL || thread_is_terminated(&current->thread)) return SIGNAL_WAIT_FAILED;

	result = signal_prepare_wait_receiver(signal, current);
	if (result != SIGNAL_OK) return result;

	for (;;) {
		struct irq_state wait_state;

		if (thread_should_cancel(&current->thread)) {
			signal_clear_waiting(signal, current);
			return SIGNAL_WAIT_CANCELED;
		}

		wait_state = spinlock_lock_irqsave(&signal->waiters.lock);
		spinlock_lock(&signal->lock);
		if (signal->closing) {
			result = SIGNAL_CLOSED;
		}
		else if ((binding = signal_find_wait_locked(signal, current, NULL)) == NULL) {
			result = SIGNAL_WAIT_FAILED;
		}
		else if (signal_take_wait_value_locked(signal, binding, out_payload)) {
			result = SIGNAL_OK;
		}
		else {
			binding->waiting = true;
			spinlock_unlock(&signal->lock);
			if (!sched_block_current_locked(&signal->waiters, THREAD_BLOCK_SIGNAL, wait_state)) {
				signal_clear_waiting(signal, current);
				return thread_should_cancel(&current->thread) ? SIGNAL_WAIT_CANCELED : SIGNAL_WAIT_FAILED;
			}
			continue;
		}
		spinlock_unlock(&signal->lock);
		spinlock_unlock_irqrestore(&signal->waiters.lock, wait_state);
		return result;
	}
}

enum signal_result signal_register_handler(struct signal* signal, struct uthread* target,
                                           user_upcall_entry_t* handler) {
	struct signal_handler_binding* binding;
	struct signal_handler_binding* existing;
	struct irq_state               state;
	enum signal_result             result         = SIGNAL_OK;
	bool                           keep_binding   = false;
	bool                           keep_reference = false;

	if (signal == NULL || target == NULL || handler == NULL) return SIGNAL_INVALID_ARGUMENTS;
	if (!uthread_retain(target)) return SIGNAL_UNAVAILABLE;

	binding = malloc(sizeof(*binding));
	if (binding != NULL) {
		*binding = (struct signal_handler_binding){
			.target = target,
			.entry  = (uintptr_t)handler,
		};
	}

	state    = spinlock_lock_irqsave(&signal->lock);
	existing = signal_find_handler_locked(signal, target, NULL);
	if (signal->closing) {
		result = SIGNAL_CLOSED;
	}
	else if (__atomic_load_n(&target->dying, __ATOMIC_ACQUIRE) != 0u) {
		result = SIGNAL_UNAVAILABLE;
	}
	else if (existing != NULL) {
		(void)uthread_upcall_purge(existing->target, USER_UPCALL_ORIGIN_SIGNAL, (uintptr_t)existing);
		existing->entry = (uintptr_t)handler;
	}
	else if (binding == NULL) {
		result = SIGNAL_NO_MEMORY;
	}
	else {
		if (signal->handler_tail == NULL) {
			signal->handler_head = binding;
		}
		else {
			signal->handler_tail->next = binding;
		}
		signal->handler_tail = binding;
		signal->handler_count++;
		keep_binding   = true;
		keep_reference = true;
	}
	spinlock_unlock_irqrestore(&signal->lock, state);

	if (!keep_binding) free(binding);
	if (!keep_reference) uthread_release(target);
	return result;
}

enum signal_result signal_unregister_handler(struct signal* signal, struct uthread* target) {
	struct signal_handler_binding* previous;
	struct signal_handler_binding* binding;
	struct irq_state               state;

	if (signal == NULL || target == NULL) return SIGNAL_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&signal->lock);
	if (signal->closing) {
		spinlock_unlock_irqrestore(&signal->lock, state);
		return SIGNAL_CLOSED;
	}
	binding = signal_find_handler_locked(signal, target, &previous);
	if (binding != NULL) (void)signal_remove_handler_locked(signal, previous, binding);
	spinlock_unlock_irqrestore(&signal->lock, state);

	if (binding == NULL) return SIGNAL_HANDLER_NOT_REGISTERED;
	signal_release_handlers(binding);
	return SIGNAL_OK;
}

void signal_unregister_thread_receivers(struct uthread* target) {
	struct signal_handler_binding* released_handlers = NULL;
	struct signal_wait_binding*    released_waits    = NULL;
	struct irq_state               table_state;

	if (target == NULL) return;

	table_state = spinlock_lock_irqsave(&signal_table.lock);
	for (size_t table_index = 0u; table_index < signal_table.capacity; table_index++) {
		struct signal*                 signal = signal_table.slots[table_index];
		struct signal_handler_binding* handler_previous;
		struct signal_handler_binding* handler;
		struct signal_wait_binding*    wait_previous;
		struct signal_wait_binding*    wait;

		if (signal == NULL) continue;
		spinlock_lock(&signal->lock);
		handler = signal_find_handler_locked(signal, target, &handler_previous);
		if (handler != NULL) {
			(void)signal_remove_handler_locked(signal, handler_previous, handler);
			handler->next     = released_handlers;
			released_handlers = handler;
		}
		wait = signal_find_wait_locked(signal, target, &wait_previous);
		if (wait != NULL) {
			(void)signal_remove_wait_locked(signal, wait_previous, wait);
			wait->next     = released_waits;
			released_waits = wait;
		}
		spinlock_unlock(&signal->lock);
	}
	spinlock_unlock_irqrestore(&signal_table.lock, table_state);

	signal_release_handlers(released_handlers);
	signal_release_waits(released_waits);
}

bool signal_has_value(struct signal* signal) {
	struct irq_state state;
	bool             has_value;

	if (signal == NULL) return false;
	state     = spinlock_lock_irqsave(&signal->lock);
	has_value = signal->has_value;
	spinlock_unlock_irqrestore(&signal->lock, state);
	return has_value;
}

uint64_t signal_generation(struct signal* signal) {
	struct irq_state state;
	uint64_t         generation;

	if (signal == NULL) return 0u;
	state      = spinlock_lock_irqsave(&signal->lock);
	generation = signal->generation;
	spinlock_unlock_irqrestore(&signal->lock, state);
	return generation;
}

size_t signal_handler_count(struct signal* signal) {
	struct irq_state state;
	size_t           count;

	if (signal == NULL) return 0u;
	state = spinlock_lock_irqsave(&signal->lock);
	count = signal->handler_count;
	spinlock_unlock_irqrestore(&signal->lock, state);
	return count;
}

size_t signal_wait_subscription_count(struct signal* signal) {
	struct irq_state state;
	size_t           count;

	if (signal == NULL) return 0u;
	state = spinlock_lock_irqsave(&signal->lock);
	count = signal->wait_receiver_count;
	spinlock_unlock_irqrestore(&signal->lock, state);
	return count;
}

size_t signal_blocked_waiter_count(struct signal* signal) {
	return signal == NULL ? 0u : thread_wait_queue_depth(&signal->waiters);
}

size_t signal_count(void) {
	return id_table_count(&signal_table);
}
