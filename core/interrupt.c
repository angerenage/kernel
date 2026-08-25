#include <base/interrupt.h>
#include <base/signal.h>
#include <core/interrupt.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct interrupt_binding {
	struct interrupt_binding* next;
	struct signal*            signal;
	signal_id_t               sid;
	process_id_t              owner;
	interrupt_id_t            id;
	size_t                    dispatch_count;
	bool                      armed;
	bool                      removing;
};

static struct interrupt_binding* bindings;
static struct spinlock           interrupt_lock =
	SPINLOCK_INIT_CLASS("interrupt", SPINLOCK_ORDER_INTERRUPT, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);

static struct interrupt_binding* binding_find_locked(interrupt_id_t id) {
	for (struct interrupt_binding* binding = bindings; binding != NULL; binding = binding->next) {
		if (binding->id == id) return binding;
	}
	return NULL;
}

static struct interrupt_binding* binding_find_signal_locked(signal_id_t signal_id) {
	for (struct interrupt_binding* binding = bindings; binding != NULL; binding = binding->next) {
		if (binding->sid == signal_id) return binding;
	}
	return NULL;
}

static struct interrupt_binding** binding_link_locked(interrupt_id_t id) {
	struct interrupt_binding** link = &bindings;

	while (*link != NULL && (*link)->id != id) link = &(*link)->next;
	return link;
}

static void binding_wait_idle(struct interrupt_binding* binding) {
	for (;;) {
		struct irq_state state;
		bool             idle;

		state = spinlock_lock_irqsave(&interrupt_lock);
		idle  = binding->dispatch_count == 0u;
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		if (idle) return;
		spinlock_relax();
	}
}

static void binding_release(struct interrupt_binding* binding) {
	if (binding == NULL) return;
	binding_wait_idle(binding);
	signal_release(binding->signal);
	free(binding);
}

enum interrupt_result interrupt_attach(process_id_t owner, interrupt_id_t id, struct signal* signal) {
	struct interrupt_binding* binding;
	struct irq_state          state;
	signal_id_t               sid;

	if (owner == PROCESS_PID_INVALID || id == INTERRUPT_ID_INVALID || signal == NULL) {
		return INTERRUPT_INVALID_ARGUMENTS;
	}
	if (!signal_retain(signal)) return INTERRUPT_UNAVAILABLE;
	sid = signal_id(signal);
	if (sid == SIGNAL_ID_INVALID) {
		signal_release(signal);
		return INTERRUPT_UNAVAILABLE;
	}
	binding = malloc(sizeof(*binding));
	if (binding == NULL) {
		signal_release(signal);
		return INTERRUPT_NO_MEMORY;
	}
	*binding = (struct interrupt_binding){
		.signal = signal,
		.sid    = sid,
		.owner  = owner,
		.id     = id,
		.armed  = true,
	};

	state = spinlock_lock_irqsave(&interrupt_lock);
	if (binding_find_locked(id) != NULL || binding_find_signal_locked(sid) != NULL) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		signal_release(signal);
		free(binding);
		return INTERRUPT_ALREADY_ATTACHED;
	}
	binding->next = bindings;
	bindings      = binding;
	if (!hal_interrupt_attach(id)) {
		bindings = binding->next;
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		signal_release(signal);
		free(binding);
		return INTERRUPT_UNAVAILABLE;
	}
	spinlock_unlock_irqrestore(&interrupt_lock, state);
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_detach(process_id_t owner, interrupt_id_t id) {
	struct interrupt_binding** link;
	struct interrupt_binding*  binding;
	struct irq_state           state;

	if (owner == PROCESS_PID_INVALID || id == INTERRUPT_ID_INVALID) return INTERRUPT_INVALID_ARGUMENTS;
	state   = spinlock_lock_irqsave(&interrupt_lock);
	link    = binding_link_locked(id);
	binding = *link;
	if (binding == NULL) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return INTERRUPT_NOT_FOUND;
	}
	if (binding->owner != owner) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return INTERRUPT_NOT_OWNER;
	}
	binding->removing = true;
	if (!hal_interrupt_detach(id)) {
		binding->removing = false;
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return INTERRUPT_UNAVAILABLE;
	}
	*link         = binding->next;
	binding->next = NULL;
	spinlock_unlock_irqrestore(&interrupt_lock, state);
	binding_release(binding);
	return INTERRUPT_OK;
}

bool interrupt_rearm_signal(signal_id_t signal_id) {
	struct interrupt_binding* binding;
	struct irq_state          state;

	if (signal_id == SIGNAL_ID_INVALID) return false;
	state   = spinlock_lock_irqsave(&interrupt_lock);
	binding = binding_find_signal_locked(signal_id);
	if (binding == NULL || binding->removing) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return false;
	}
	if (!binding->armed) {
		if (!hal_interrupt_rearm(binding->id)) {
			spinlock_unlock_irqrestore(&interrupt_lock, state);
			return false;
		}
		binding->armed = true;
	}
	spinlock_unlock_irqrestore(&interrupt_lock, state);
	return true;
}

bool interrupt_dispatch(interrupt_id_t id) {
	struct interrupt_binding* binding;
	struct irq_state          state;
	struct signal_payload     payload = {
			.args = {(uint64_t)id, 0u, 0u, 0u}
    };
	struct signal* signal;

	if (id == INTERRUPT_ID_INVALID) return false;
	state   = spinlock_lock_irqsave(&interrupt_lock);
	binding = binding_find_locked(id);
	if (binding == NULL) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return false;
	}
	if (binding->removing || !binding->armed) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return true;
	}
	if (!hal_interrupt_mask(id)) {
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		return false;
	}
	binding->armed = false;
	if (binding->dispatch_count == SIZE_MAX) hcf();
	binding->dispatch_count++;
	signal = binding->signal;
	spinlock_unlock_irqrestore(&interrupt_lock, state);

	(void)signal_send_force(signal, SIGNAL_SENDER_KERNEL, &payload, NULL, NULL);

	state = spinlock_lock_irqsave(&interrupt_lock);
	if (binding->dispatch_count == 0u) hcf();
	binding->dispatch_count--;
	spinlock_unlock_irqrestore(&interrupt_lock, state);
	return true;
}

void interrupt_cleanup_process(process_id_t owner) {
	if (owner == PROCESS_PID_INVALID) return;
	for (;;) {
		struct interrupt_binding** link;
		struct interrupt_binding*  binding = NULL;
		struct irq_state           state   = spinlock_lock_irqsave(&interrupt_lock);

		for (link = &bindings; *link != NULL; link = &(*link)->next) {
			if ((*link)->owner != owner) continue;
			binding           = *link;
			binding->removing = true;
			(void)hal_interrupt_detach(binding->id);
			*link         = binding->next;
			binding->next = NULL;
			break;
		}
		spinlock_unlock_irqrestore(&interrupt_lock, state);
		if (binding == NULL) return;
		binding_release(binding);
	}
}
