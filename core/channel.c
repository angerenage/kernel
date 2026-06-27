#include <base/cap.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/channel.h>
#include <core/id_table.h>
#include <core/ring_buffer.h>
#include <core/spinlock.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <string.h>

static struct id_table channel_table = {
	.lock    = SPINLOCK_INIT_CLASS("channel_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = 1u,
	.min_id  = 1u,
	.max_id  = UINT64_MAX,
};

struct channel* channel_create(process_id_t owner_pid) {
	struct channel*      ch;
	channel_id_t         id;
	enum id_table_result id_result;

	if (owner_pid == PROCESS_PID_INVALID) return NULL;

	ch = malloc(sizeof(*ch));
	if (ch == NULL) return NULL;
	memset(ch, 0, sizeof(*ch));
	spinlock_init_class(&ch->lock, "channel_lock", SPINLOCK_ORDER_MUTEX, SPINLOCK_FLAG_IRQSAVE);
	ch->owner_pid       = owner_pid;
	ch->reference_count = 1u;
	if (!ring_buffer_init(&ch->cap_queue,
	                      "channel_cap_queue",
	                      SPINLOCK_ORDER_ID_TABLE,
	                      CAP_REQUEST_QUEUE_DEPTH,
	                      sizeof(struct cap_request))) {
		free(ch);
		return NULL;
	}

	id_result = id_table_alloc(&channel_table, ch, &id);
	if (id_result != ID_TABLE_OK) {
		ring_buffer_deinit(&ch->cap_queue);
		free(ch);
		return NULL;
	}
	ch->id = (channel_id_t)id;

	return ch;
}

enum channel_result channel_destroy(struct channel* channel, process_id_t caller_pid) {
	struct irq_state state;

	if (channel == NULL) return CHANNEL_INVALID_ARGUMENTS;
	if (caller_pid != channel->owner_pid) return CHANNEL_NOT_OWNER;

	state = spinlock_lock_irqsave(&channel->lock);
	if (channel->closing) {
		spinlock_unlock_irqrestore(&channel->lock, state);
		return CHANNEL_NOT_FOUND;
	}
	channel->closing = true;
	spinlock_unlock_irqrestore(&channel->lock, state);
	if (id_table_remove(&channel_table, (id_table_id_t)channel->id, NULL) != ID_TABLE_OK) {
		return CHANNEL_NOT_FOUND;
	}
	cap_object_unregister_endpoint(channel);
	cap_pending_call_cancel_channel(channel->id);
	channel_release(channel);
	return CHANNEL_OK;
}

struct channel* channel_lookup(channel_id_t id) {
	return (struct channel*)id_table_lookup(&channel_table, (id_table_id_t)id);
}

static bool channel_retain_callback(void* value, void* context) {
	struct channel* channel = value;
	(void)context;

	if (__atomic_load_n(&channel->closing, __ATOMIC_ACQUIRE)) return false;
	(void)__atomic_add_fetch(&channel->reference_count, 1u, __ATOMIC_RELAXED);
	return true;
}

struct channel* channel_acquire(channel_id_t id) {
	if (id == CHANNEL_ID_INVALID) return NULL;
	return id_table_lookup_retain(&channel_table, id, channel_retain_callback, NULL);
}

bool channel_retain(struct channel* channel) {
	struct irq_state state;

	if (channel == NULL) return true;
	state = spinlock_lock_irqsave(&channel->lock);
	if (channel->closing) {
		spinlock_unlock_irqrestore(&channel->lock, state);
		return false;
	}
	(void)__atomic_add_fetch(&channel->reference_count, 1u, __ATOMIC_RELAXED);
	spinlock_unlock_irqrestore(&channel->lock, state);
	return true;
}

void channel_release(struct channel* channel) {
	if (channel == NULL) return;
	if (__atomic_sub_fetch(&channel->reference_count, 1u, __ATOMIC_ACQ_REL) != 0u) return;
	ring_buffer_deinit(&channel->cap_queue);
	free(channel);
}

bool channel_enqueue_cap_request(struct channel* channel, const struct cap_request* request) {
	struct irq_state state;
	bool             enqueued;

	if (channel == NULL || request == NULL) return false;
	state = spinlock_lock_irqsave(&channel->lock);
	if (channel->closing) {
		spinlock_unlock_irqrestore(&channel->lock, state);
		return false;
	}
	enqueued = ring_buffer_enqueue(&channel->cap_queue, request);
	spinlock_unlock_irqrestore(&channel->lock, state);
	return enqueued;
}

void process_channel_state_init(struct process_channel_state* state) {
	if (state == NULL) return;
	memset(state, 0, sizeof(*state));
	spinlock_init_class(&state->lock, "process_channels", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);
}

void process_channel_state_deinit(struct process_channel_state* state) {
	if (state == NULL) return;
	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		struct channel*  channel;
		struct irq_state irq_state = spinlock_lock_irqsave(&state->lock);

		channel            = state->channels[i];
		state->channels[i] = NULL;
		if (channel != NULL && state->count > 0u) state->count--;
		spinlock_unlock_irqrestore(&state->lock, irq_state);

		if (channel != NULL) (void)channel_destroy(channel, channel->owner_pid);
	}
}

bool process_channel_state_add(struct process_channel_state* state, struct channel* channel) {
	struct irq_state irq_state;
	bool             added = false;

	if (state == NULL || channel == NULL) return false;
	irq_state = spinlock_lock_irqsave(&state->lock);
	if (state->count >= CHANNEL_MAX_PER_PROCESS) {
		spinlock_unlock_irqrestore(&state->lock, irq_state);
		return false;
	}

	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		if (state->channels[i] == NULL) {
			state->channels[i] = channel;
			state->count++;
			added = true;
			break;
		}
	}
	spinlock_unlock_irqrestore(&state->lock, irq_state);
	return added;
}

void process_channel_state_remove(struct process_channel_state* state, struct channel* channel) {
	struct irq_state irq_state;

	if (state == NULL || channel == NULL) return;
	irq_state = spinlock_lock_irqsave(&state->lock);
	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		if (state->channels[i] == channel) {
			state->channels[i] = NULL;
			if (state->count > 0u) state->count--;
			break;
		}
	}
	spinlock_unlock_irqrestore(&state->lock, irq_state);
}
