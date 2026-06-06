#pragma once

#include <base/channel.h>
#include <base/process.h>
#include <core/message.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A channel is a kernel object wrapping a message queue. It has a single
 * owner process that may receive from or destroy it. Any process that knows
 * the channel ID may send to it. Channel IDs are plain integers that can be
 * carried in any message payload like any other data.
 */
struct channel {
	channel_id_t         id;
	struct message_queue queue;
	process_id_t         owner_pid;
};

#define CHANNEL_MAX_PER_PROCESS 64u

struct process_channel_state {
	size_t          count;
	struct channel* channels[CHANNEL_MAX_PER_PROCESS];
};

/* Create a new channel owned by the given process. Returns NULL for an invalid owner PID or allocation failure. */
struct channel* channel_create(process_id_t owner_pid);

/* Destroy a channel. Only the owner may do this. */
enum channel_result channel_destroy(struct channel* channel, process_id_t caller_pid);

/* Send a message to a channel (any process may do this). */
enum channel_result channel_send(struct channel* channel, process_id_t sender_pid, const void* data, size_t length);

/* Receive a message from a channel (only the owner may do this). */
enum channel_result channel_recv(struct channel* channel, process_id_t caller_pid, void* buffer, size_t buffer_size,
                                 size_t* out_length, process_id_t* out_sender_pid);

/* Look up a channel by its global ID. */
struct channel* channel_lookup(channel_id_t id);

/* Initialize per-process channel state. */
void process_channel_state_init(struct process_channel_state* state);

/* Deinitialize per-process channel state, destroying all owned channels. */
void process_channel_state_deinit(struct process_channel_state* state);

/* Register a newly-created channel in the process's owned-channel list. Returns false if the limit is reached. */
bool process_channel_state_add(struct process_channel_state* state, struct channel* channel);

/* Remove a channel from the process's owned-channel list. */
void process_channel_state_remove(struct process_channel_state* state, struct channel* channel);
