#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <core/ring_buffer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A channel is a kernel object that carries capability call requests. It has
 * a single owner process that may receive from or destroy it. Any process that
 * knows the channel ID may send capability calls to it.
 */
struct channel {
	channel_id_t       id;
	struct ring_buffer cap_queue;
	process_id_t       owner_pid;
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
