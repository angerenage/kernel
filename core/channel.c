#include <core/channel.h>
#include <core/id_table.h>
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
	message_queue_init(&ch->queue);
	ch->owner_pid = owner_pid;

	id_result = id_table_alloc(&channel_table, ch, &id);
	if (id_result != ID_TABLE_OK) {
		free(ch);
		return NULL;
	}
	ch->id = (channel_id_t)id;

	return ch;
}

enum channel_result channel_destroy(struct channel* channel, process_id_t caller_pid) {
	if (channel == NULL) return CHANNEL_INVALID_ARGUMENTS;
	if (caller_pid != channel->owner_pid) return CHANNEL_NOT_OWNER;

	(void)id_table_remove(&channel_table, (id_table_id_t)channel->id, NULL);
	memset(channel, 0, sizeof(*channel));
	free(channel);
	return CHANNEL_OK;
}

enum channel_result channel_send(struct channel* channel, process_id_t sender_pid, const void* data, size_t length) {
	if (channel == NULL) return CHANNEL_INVALID_ARGUMENTS;

	enum message_result result = message_queue_send(&channel->queue, sender_pid, data, length);
	switch (result) {
	case MESSAGE_OK:
		return CHANNEL_OK;
	case MESSAGE_INVALID_ARGUMENTS:
		return CHANNEL_INVALID_ARGUMENTS;
	case MESSAGE_TOO_LARGE:
		return CHANNEL_INVALID_ARGUMENTS;
	case MESSAGE_QUEUE_FULL:
		return CHANNEL_QUEUE_FULL;
	case MESSAGE_NO_MESSAGE:
	default:
		return CHANNEL_INVALID_ARGUMENTS;
	}
}

enum channel_result channel_recv(struct channel* channel, process_id_t caller_pid, void* buffer, size_t buffer_size,
                                 size_t* out_length, process_id_t* out_sender_pid) {
	if (channel == NULL) return CHANNEL_INVALID_ARGUMENTS;
	if (caller_pid != channel->owner_pid) return CHANNEL_NOT_OWNER;

	enum message_result result =
		message_queue_receive(&channel->queue, buffer, buffer_size, out_length, out_sender_pid);
	switch (result) {
	case MESSAGE_OK:
		return CHANNEL_OK;
	case MESSAGE_NO_MESSAGE:
		return CHANNEL_NO_MESSAGE;
	case MESSAGE_TOO_LARGE:
		return CHANNEL_BUFFER_TOO_SMALL;
	case MESSAGE_INVALID_ARGUMENTS:
	default:
		return CHANNEL_INVALID_ARGUMENTS;
	}
}

struct channel* channel_lookup(channel_id_t id) {
	return (struct channel*)id_table_lookup(&channel_table, (id_table_id_t)id);
}

void process_channel_state_init(struct process_channel_state* state) {
	if (state == NULL) return;
	memset(state, 0, sizeof(*state));
}

void process_channel_state_deinit(struct process_channel_state* state) {
	if (state == NULL) return;
	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		if (state->channels[i] != NULL) {
			channel_destroy(state->channels[i], state->channels[i]->owner_pid);
			state->channels[i] = NULL;
		}
	}
	state->count = 0u;
}

bool process_channel_state_add(struct process_channel_state* state, struct channel* channel) {
	if (state == NULL || channel == NULL) return false;
	if (state->count >= CHANNEL_MAX_PER_PROCESS) return false;

	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		if (state->channels[i] == NULL) {
			state->channels[i] = channel;
			state->count++;
			return true;
		}
	}
	return false;
}

void process_channel_state_remove(struct process_channel_state* state, struct channel* channel) {
	if (state == NULL || channel == NULL) return;
	for (size_t i = 0u; i < CHANNEL_MAX_PER_PROCESS; i++) {
		if (state->channels[i] == channel) {
			state->channels[i] = NULL;
			if (state->count > 0u) state->count--;
			break;
		}
	}
}
