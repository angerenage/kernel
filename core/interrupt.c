#include <base/signal.h>
#include <core/cpu.h>
#include <core/interrupt.h>
#include <core/signal.h>
#include <core/spinlock.h>
#include <libc/stdlib.h>
#include <string.h>

struct interrupt {
	struct spinlock                     lock;
	struct interrupt*                   next;
	enum interrupt_kind                 kind;
	struct signal*                      signal;
	signal_id_t                         signal_id;
	struct hal_interrupt_event          event;
	struct hal_interrupt_delivery_range delivery;
	struct hal_interrupt_source         source;
	struct hal_interrupt_source_state   source_state;
	struct hal_interrupt_message_state  message_state;
	uint64_t                            references;
	size_t                              dispatch_count;
	bool                                bound;
	bool                                pending;
	bool                                retriggered;
	bool                                destroying;
};

/* Message events remain reserved after teardown until a context-specific revocation protocol exists. */
struct interrupt_quarantine {
	struct interrupt_quarantine* next;
	struct hal_interrupt_event   event;
};

struct interrupt_source_registration {
	struct interrupt_source_registration* next;
	interrupt_source_t                    token;
	struct hal_interrupt_source           source;
	enum hal_interrupt_trigger            trigger;
	enum hal_interrupt_polarity           polarity;
};

static struct spinlock interrupt_table_lock = SPINLOCK_INIT_CLASS(
	"interrupt_table", SPINLOCK_ORDER_INTERRUPT, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
static struct interrupt*                     interrupt_table;
static struct interrupt_quarantine*          interrupt_quarantine;
static struct interrupt_source_registration* interrupt_sources;
static interrupt_source_t                    interrupt_next_source_token = 1u;
static bool                                  interrupt_initialized;

static bool event_equal(struct hal_interrupt_event left, struct hal_interrupt_event right) {
	return left.domain == right.domain && left.id == right.id;
}

static bool source_equal(struct hal_interrupt_source left, struct hal_interrupt_source right) {
	return left.domain == right.domain && left.number == right.number;
}

static bool source_from_token(interrupt_source_t token, struct hal_interrupt_source* out_source,
                              enum hal_interrupt_trigger* out_trigger, enum hal_interrupt_polarity* out_polarity) {
	if (out_source == NULL || out_trigger == NULL || out_polarity == NULL || token == INTERRUPT_SOURCE_INVALID)
		return false;
	for (struct interrupt_source_registration* item = interrupt_sources; item != NULL; item = item->next) {
		if (item->token != token) continue;
		*out_source   = item->source;
		*out_trigger  = item->trigger;
		*out_polarity = item->polarity;
		return true;
	}
	return false;
}

static const struct cpu* choose_source_target(const struct hal_interrupt_source*      source,
                                              const struct hal_interrupt_source_info* info) {
	struct cpu* current;

	if (info->target_kind == HAL_INTERRUPT_TARGET_FIXED) return info->fixed_target;
	current = cpu_current();
	if (current != NULL && hal_interrupt_source_target_supported(source, current)) return current;
	for (size_t index = 0u; index < cpu_count(); index++) {
		struct cpu* cpu = cpu_by_index(index);
		if (cpu != NULL && cpu->state == CPU_STATE_ONLINE && hal_interrupt_source_target_supported(source, cpu))
			return cpu;
	}
	return NULL;
}

static const struct cpu* choose_message_target(uint32_t domain, const struct hal_interrupt_message_source* source) {
	struct cpu* current = cpu_current();
	if (current != NULL && hal_interrupt_message_target_supported(domain, source, current)) return current;
	for (size_t index = 0u; index < cpu_count(); index++) {
		struct cpu* cpu = cpu_by_index(index);
		if (cpu != NULL && cpu->state == CPU_STATE_ONLINE &&
		    hal_interrupt_message_target_supported(domain, source, cpu))
			return cpu;
	}
	return NULL;
}

static bool event_allocated_locked(struct hal_interrupt_event event) {
	for (struct interrupt* item = interrupt_table; item != NULL; item = item->next) {
		if (!item->destroying && event_equal(item->event, event)) return true;
	}
	for (struct interrupt_quarantine* item = interrupt_quarantine; item != NULL; item = item->next) {
		if (event_equal(item->event, event)) return true;
	}
	return false;
}

static bool allocate_event_locked(struct hal_interrupt_delivery_range range, struct hal_interrupt_event* out_event) {
	if (out_event == NULL || range.base >= range.limit) return false;
	for (uint32_t id = range.base; id < range.limit; id++) {
		struct hal_interrupt_event event = {.domain = range.domain, .id = id};
		if (!event_allocated_locked(event)) {
			*out_event = event;
			return true;
		}
	}
	return false;
}

static bool interrupt_prepare_publication_locked(struct interrupt* interrupt, struct signal** out_signal) {
	if (interrupt == NULL || out_signal == NULL || interrupt->signal == NULL || interrupt->references == UINT64_MAX ||
	    __atomic_load_n(&interrupt->dispatch_count, __ATOMIC_RELAXED) == SIZE_MAX || !signal_retain(interrupt->signal))
		return false;
	interrupt->references++;
	(void)__atomic_add_fetch(&interrupt->dispatch_count, 1u, __ATOMIC_RELAXED);
	*out_signal = interrupt->signal;
	return true;
}

static void interrupt_finish_publication_locked(struct interrupt* interrupt) {
	if (__atomic_load_n(&interrupt->dispatch_count, __ATOMIC_RELAXED) == 0u) return;
	(void)__atomic_sub_fetch(&interrupt->dispatch_count, 1u, __ATOMIC_RELEASE);
}

static void interrupt_synchronize_dispatch(struct interrupt* interrupt) {
	while (__atomic_load_n(&interrupt->dispatch_count, __ATOMIC_ACQUIRE) != 0u) spinlock_relax();
}

bool interrupt_init(void) {
	struct irq_state state = spinlock_lock_irqsave(&interrupt_table_lock);
	interrupt_initialized  = true;
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);
	return true;
}

enum interrupt_result interrupt_register_source(const struct hal_interrupt_source* source,
                                                enum hal_interrupt_trigger         trigger,
                                                enum hal_interrupt_polarity polarity, interrupt_source_t* out_token) {
	struct interrupt_source_registration* registration;
	struct hal_interrupt_source_info      info;
	struct irq_state                      state;

	if (source == NULL || out_token == NULL || trigger > HAL_INTERRUPT_TRIGGER_LEVEL ||
	    polarity > HAL_INTERRUPT_POLARITY_LOW || !hal_interrupt_source_info(source, &info))
		return INTERRUPT_INVALID_ARGUMENTS;
	*out_token   = INTERRUPT_SOURCE_INVALID;
	registration = malloc(sizeof(*registration));
	if (registration == NULL) return INTERRUPT_NO_MEMORY;
	state = spinlock_lock_irqsave(&interrupt_table_lock);
	if (!interrupt_initialized) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(registration);
		return INTERRUPT_UNAVAILABLE;
	}
	for (struct interrupt_source_registration* item = interrupt_sources; item != NULL; item = item->next) {
		if (!source_equal(item->source, *source)) continue;
		if (item->trigger == trigger && item->polarity == polarity) {
			*out_token = item->token;
			spinlock_unlock_irqrestore(&interrupt_table_lock, state);
			free(registration);
			return INTERRUPT_OK;
		}
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(registration);
		return INTERRUPT_INVALID_ARGUMENTS;
	}
	if (interrupt_next_source_token == INTERRUPT_SOURCE_INVALID) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(registration);
		return INTERRUPT_UNAVAILABLE;
	}
	*registration     = (struct interrupt_source_registration){.next     = interrupt_sources,
	                                                           .token    = interrupt_next_source_token++,
	                                                           .source   = *source,
	                                                           .trigger  = trigger,
	                                                           .polarity = polarity};
	interrupt_sources = registration;
	*out_token        = registration->token;
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);
	return INTERRUPT_OK;
}

bool interrupt_retain(struct interrupt* interrupt) {
	struct irq_state state;
	if (interrupt == NULL) return false;
	state = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->destroying || interrupt->references == UINT64_MAX) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return false;
	}
	interrupt->references++;
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	return true;
}

void interrupt_release(struct interrupt* interrupt) {
	struct irq_state state;
	bool             free_interrupt = false;
	if (interrupt == NULL) return;
	state = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->references != 0u && --interrupt->references == 0u) free_interrupt = true;
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	if (free_interrupt) free(interrupt);
}

enum interrupt_result interrupt_claim_source(interrupt_source_t token, struct interrupt** out_interrupt) {
	struct hal_interrupt_source      source;
	struct hal_interrupt_source_info info;
	enum hal_interrupt_trigger       trigger;
	enum hal_interrupt_polarity      polarity;
	const struct cpu*                target;
	struct interrupt*                interrupt;
	struct irq_state                 state;

	if (out_interrupt == NULL) return INTERRUPT_INVALID_ARGUMENTS;
	*out_interrupt    = NULL;
	state             = spinlock_lock_irqsave(&interrupt_table_lock);
	bool source_found = source_from_token(token, &source, &trigger, &polarity);
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);
	if (!source_found || !hal_interrupt_source_info(&source, &info)) return INTERRUPT_NOT_FOUND;
	target = choose_source_target(&source, &info);
	if (target == NULL) return INTERRUPT_UNAVAILABLE;
	interrupt = calloc(1u, sizeof(*interrupt));
	if (interrupt == NULL) return INTERRUPT_NO_MEMORY;
	spinlock_init_class(
		&interrupt->lock, "interrupt", SPINLOCK_ORDER_INTERRUPT, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	interrupt->kind       = INTERRUPT_KIND_SOURCE;
	interrupt->source     = source;
	interrupt->delivery   = info.delivery;
	interrupt->references = 1u;

	state = spinlock_lock_irqsave(&interrupt_table_lock);
	if (!interrupt_initialized) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(interrupt);
		return INTERRUPT_UNAVAILABLE;
	}
	for (struct interrupt* item = interrupt_table; item != NULL; item = item->next) {
		if (item->kind == INTERRUPT_KIND_SOURCE && !item->destroying && source_equal(item->source, source)) {
			spinlock_unlock_irqrestore(&interrupt_table_lock, state);
			free(interrupt);
			return INTERRUPT_ALREADY_CLAIMED;
		}
	}
	if (!allocate_event_locked(info.delivery, &interrupt->event)) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(interrupt);
		return INTERRUPT_UNAVAILABLE;
	}
	interrupt->next = interrupt_table;
	interrupt_table = interrupt;
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);

	struct hal_interrupt_delivery delivery = {
		.target = target, .event = interrupt->event, .trigger = trigger, .polarity = polarity};
	if (!hal_interrupt_source_init(&interrupt->source_state, &source, &delivery)) {
		(void)interrupt_destroy(interrupt);
		interrupt_release(interrupt);
		return INTERRUPT_FAILED;
	}
	*out_interrupt = interrupt;
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_allocate_message(interrupt_message_context_t context, struct interrupt** out_interrupt,
                                                 struct interrupt_message* out_message) {
	struct hal_interrupt_message_range range;
	struct interrupt*                  interrupt;
	struct interrupt_quarantine*       quarantine;
	struct irq_state                   state;
	uint32_t                           domain;
	const struct cpu*                  target       = NULL;
	bool                               found_domain = false;
	bool                               allocated    = false;
	size_t                             range_count;

	if (out_interrupt == NULL || out_message == NULL || context == INTERRUPT_MESSAGE_CONTEXT_INVALID)
		return INTERRUPT_INVALID_ARGUMENTS;
	*out_interrupt = NULL;
	domain         = (uint32_t)(context >> 32u);
	range_count    = hal_interrupt_message_range_count();
	interrupt      = calloc(1u, sizeof(*interrupt));
	if (interrupt == NULL) return INTERRUPT_NO_MEMORY;
	quarantine = calloc(1u, sizeof(*quarantine));
	if (quarantine == NULL) {
		free(interrupt);
		return INTERRUPT_NO_MEMORY;
	}
	spinlock_init_class(
		&interrupt->lock, "interrupt", SPINLOCK_ORDER_INTERRUPT, SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	interrupt->kind       = INTERRUPT_KIND_MESSAGE;
	interrupt->references = 1u;
	state                 = spinlock_lock_irqsave(&interrupt_table_lock);
	if (!interrupt_initialized) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		free(quarantine);
		free(interrupt);
		return INTERRUPT_UNAVAILABLE;
	}
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);

	for (size_t index = 0u; index < range_count; index++) {
		if (!hal_interrupt_message_range_at(index, &range) || range.domain != domain) continue;
		found_domain = true;
		target       = choose_message_target(range.domain, NULL);
		if (target == NULL) continue;
		state = spinlock_lock_irqsave(&interrupt_table_lock);
		if (interrupt_initialized && allocate_event_locked(range.delivery, &interrupt->event)) {
			interrupt->delivery = range.delivery;
			interrupt->next     = interrupt_table;
			interrupt_table     = interrupt;
			allocated           = true;
		}
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		if (allocated) break;
	}
	if (!allocated) {
		free(quarantine);
		free(interrupt);
		return found_domain ? INTERRUPT_UNAVAILABLE : INTERRUPT_NOT_FOUND;
	}
	struct hal_interrupt_message         request_message;
	struct hal_interrupt_message_request request = {
		.domain = range.domain, .source = NULL, .target = target, .event = interrupt->event};
	if (!hal_interrupt_message_init(&interrupt->message_state, &request, &request_message)) {
		(void)interrupt_destroy(interrupt);
		interrupt_release(interrupt);
		free(quarantine);
		return INTERRUPT_FAILED;
	}
	/* Keep this node even after destruction: stale device messages must not reach a recycled endpoint. */
	quarantine->event    = interrupt->event;
	state                = spinlock_lock_irqsave(&interrupt_table_lock);
	quarantine->next     = interrupt_quarantine;
	interrupt_quarantine = quarantine;
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);
	*out_message =
		(struct interrupt_message){.message_address = request_message.address, .message_data = request_message.data};
	*out_interrupt = interrupt;
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_get_info(struct interrupt* interrupt, struct interrupt_info* out_info) {
	struct irq_state state;
	if (interrupt == NULL || out_info == NULL) return INTERRUPT_INVALID_ARGUMENTS;
	state = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->destroying) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return INTERRUPT_UNAVAILABLE;
	}
	*out_info = (struct interrupt_info){.kind = interrupt->kind, .bound = interrupt->bound};
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_bind(struct interrupt* interrupt, struct signal* signal) {
	struct irq_state table_state;
	struct irq_state state;
	bool             already_bound;
	bool             release_binding;
	if (interrupt == NULL || signal == NULL || !interrupt_retain(interrupt)) return INTERRUPT_INVALID_ARGUMENTS;
	if (!signal_retain(signal)) {
		interrupt_release(interrupt);
		return INTERRUPT_INVALID_ARGUMENTS;
	}
	table_state = spinlock_lock_irqsave(&interrupt_table_lock);
	state       = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->destroying || interrupt->bound) {
		already_bound = interrupt->bound;
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
		signal_release(signal);
		interrupt_release(interrupt);
		return already_bound ? INTERRUPT_ALREADY_BOUND : INTERRUPT_UNAVAILABLE;
	}
	if (!signal_bind_interrupt(signal, interrupt)) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
		signal_release(signal);
		interrupt_release(interrupt);
		return INTERRUPT_ALREADY_BOUND;
	}
	interrupt->signal    = signal;
	interrupt->signal_id = signal_id(signal);
	interrupt->bound     = true;
	if (interrupt->kind == INTERRUPT_KIND_SOURCE && !hal_interrupt_source_unmask(&interrupt->source_state)) {
		interrupt->bound     = false;
		interrupt->signal    = NULL;
		interrupt->signal_id = SIGNAL_ID_INVALID;
		release_binding      = signal_unbind_interrupt(signal, interrupt);
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
		signal_release(signal);
		if (release_binding) interrupt_release(interrupt);
		return INTERRUPT_FAILED;
	}
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_unbind(struct interrupt* interrupt) {
	struct signal*   signal;
	struct irq_state state;
	bool             release_binding;
	if (interrupt == NULL) return INTERRUPT_INVALID_ARGUMENTS;
	state = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->destroying) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return INTERRUPT_UNAVAILABLE;
	}
	if (!interrupt->bound) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return INTERRUPT_NOT_BOUND;
	}
	if (interrupt->kind == INTERRUPT_KIND_SOURCE && !hal_interrupt_source_mask(&interrupt->source_state)) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return INTERRUPT_FAILED;
	}
	signal               = interrupt->signal;
	release_binding      = signal_unbind_interrupt(signal, interrupt);
	interrupt->signal    = NULL;
	interrupt->signal_id = SIGNAL_ID_INVALID;
	interrupt->bound = interrupt->pending = interrupt->retriggered = false;
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	interrupt_synchronize_dispatch(interrupt);
	signal_release(signal);
	if (release_binding) interrupt_release(interrupt);
	return INTERRUPT_OK;
}

enum interrupt_result interrupt_destroy(struct interrupt* interrupt) {
	struct interrupt** link;
	struct signal*     signal = NULL;
	struct irq_state   state;
	bool               release_binding = false;
	if (interrupt == NULL) return INTERRUPT_INVALID_ARGUMENTS;
	state = spinlock_lock_irqsave(&interrupt_table_lock);
	spinlock_lock(&interrupt->lock);
	if (interrupt->destroying) {
		spinlock_unlock(&interrupt->lock);
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		return INTERRUPT_UNAVAILABLE;
	}
	interrupt->destroying = true;
	if (interrupt->kind == INTERRUPT_KIND_SOURCE && !hal_interrupt_source_deinit(&interrupt->source_state)) {
		interrupt->destroying = false;
		spinlock_unlock(&interrupt->lock);
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		return INTERRUPT_FAILED;
	}
	if (interrupt->kind == INTERRUPT_KIND_MESSAGE && !hal_interrupt_message_deinit(&interrupt->message_state)) {
		interrupt->destroying = false;
		spinlock_unlock(&interrupt->lock);
		spinlock_unlock_irqrestore(&interrupt_table_lock, state);
		return INTERRUPT_FAILED;
	}
	for (link = &interrupt_table; *link != NULL && *link != interrupt; link = &(*link)->next) {
	}
	if (*link == interrupt) *link = interrupt->next;
	signal = interrupt->signal;
	if (signal != NULL) release_binding = signal_unbind_interrupt(signal, interrupt);
	interrupt->signal    = NULL;
	interrupt->signal_id = SIGNAL_ID_INVALID;
	interrupt->bound     = false;
	spinlock_unlock(&interrupt->lock);
	spinlock_unlock_irqrestore(&interrupt_table_lock, state);
	interrupt_synchronize_dispatch(interrupt);
	if (signal != NULL) signal_release(signal);
	if (release_binding) interrupt_release(interrupt);
	return INTERRUPT_OK;
}

bool interrupt_signal_destroying(struct interrupt* interrupt, struct signal* signal) {
	struct irq_state state;
	bool             detached = false;

	if (interrupt == NULL || signal == NULL) return false;
	state = spinlock_lock_irqsave(&interrupt->lock);
	if (interrupt->signal != signal) {
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return true;
	}
	if (interrupt->kind == INTERRUPT_KIND_SOURCE && !hal_interrupt_source_mask(&interrupt->source_state)) {
		if (!signal_restore_interrupt(signal, interrupt)) {
			spinlock_unlock_irqrestore(&interrupt->lock, state);
			return false;
		}
		spinlock_unlock_irqrestore(&interrupt->lock, state);
		return false;
	}
	interrupt->signal      = NULL;
	interrupt->signal_id   = SIGNAL_ID_INVALID;
	interrupt->bound       = false;
	interrupt->pending     = false;
	interrupt->retriggered = false;
	detached               = true;
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	interrupt_synchronize_dispatch(interrupt);
	if (detached) signal_release(signal);
	return true;
}

bool interrupt_handle_event(struct hal_interrupt_event event) {
	struct interrupt*     interrupt = NULL;
	struct signal*        signal    = NULL;
	struct signal_payload payload   = {0};
	enum signal_result    send_result;
	struct irq_state      table_state = spinlock_lock_irqsave(&interrupt_table_lock);
	for (struct interrupt* item = interrupt_table; item != NULL; item = item->next) {
		if (event_equal(item->event, event)) {
			interrupt = item;
			break;
		}
	}
	if (interrupt == NULL) {
		spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
		return false;
	}
	spinlock_lock(&interrupt->lock);
	spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
	if (interrupt->destroying || !interrupt->bound) {
		spinlock_unlock(&interrupt->lock);
		return true;
	}
	if (interrupt->kind == INTERRUPT_KIND_SOURCE) {
		if (interrupt->pending) {
			spinlock_unlock(&interrupt->lock);
			return true;
		}
		if (!hal_interrupt_source_mask(&interrupt->source_state)) {
			spinlock_unlock(&interrupt->lock);
			return true;
		}
		interrupt->pending = true;
	}
	else if (interrupt->pending) {
		interrupt->retriggered = true;
		spinlock_unlock(&interrupt->lock);
		return true;
	}
	else interrupt->pending = true;
	if (!interrupt_prepare_publication_locked(interrupt, &signal)) {
		if (interrupt->kind == INTERRUPT_KIND_SOURCE) {
			if (hal_interrupt_source_unmask(&interrupt->source_state)) interrupt->pending = false;
		}
		else {
			interrupt->pending     = false;
			interrupt->retriggered = false;
		}
		spinlock_unlock(&interrupt->lock);
		return true;
	}
	spinlock_unlock(&interrupt->lock);
	send_result            = signal_send_force(signal, SIGNAL_SENDER_KERNEL, &payload, NULL, NULL);
	struct irq_state state = spinlock_lock_irqsave(&interrupt->lock);
	interrupt_finish_publication_locked(interrupt);
	if (send_result != SIGNAL_OK && !interrupt->destroying && interrupt->bound && interrupt->signal == signal) {
		if (interrupt->kind == INTERRUPT_KIND_SOURCE) {
			if (hal_interrupt_source_unmask(&interrupt->source_state)) interrupt->pending = false;
		}
		else {
			interrupt->pending     = false;
			interrupt->retriggered = false;
		}
	}
	spinlock_unlock_irqrestore(&interrupt->lock, state);
	signal_release(signal);
	interrupt_release(interrupt);
	return true;
}

void interrupt_signal_ready(signal_id_t signal_id) {
	struct signal*   signal      = NULL;
	bool             publish     = false;
	struct irq_state table_state = spinlock_lock_irqsave(&interrupt_table_lock);
	for (struct interrupt* interrupt = interrupt_table; interrupt != NULL; interrupt = interrupt->next) {
		spinlock_lock(&interrupt->lock);
		if (interrupt->signal_id != signal_id) {
			spinlock_unlock(&interrupt->lock);
			continue;
		}
		spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
		if (interrupt->bound && interrupt->pending) {
			if (interrupt->kind == INTERRUPT_KIND_SOURCE) {
				if (hal_interrupt_source_unmask(&interrupt->source_state)) interrupt->pending = false;
			}
			else if (interrupt->retriggered) {
				if (interrupt_prepare_publication_locked(interrupt, &signal)) {
					interrupt->retriggered = false;
					publish                = true;
				}
			}
			else interrupt->pending = false;
		}
		spinlock_unlock(&interrupt->lock);
		if (publish) {
			struct signal_payload payload = {0};
			enum signal_result    result  = signal_send_force(signal, SIGNAL_SENDER_KERNEL, &payload, NULL, NULL);
			struct irq_state      state   = spinlock_lock_irqsave(&interrupt->lock);
			interrupt_finish_publication_locked(interrupt);
			if (result != SIGNAL_OK && !interrupt->destroying && interrupt->bound && interrupt->signal == signal &&
			    interrupt->pending)
				interrupt->retriggered = true;
			spinlock_unlock_irqrestore(&interrupt->lock, state);
			signal_release(signal);
			interrupt_release(interrupt);
		}
		return;
	}
	spinlock_unlock_irqrestore(&interrupt_table_lock, table_state);
}
