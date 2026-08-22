#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* Create a channel and optionally return a capability for its activity signal. */
syscall_status_t channel_create(channel_id_t* out_id, cap_id_t* out_activity_signal);

/* Destroy a channel owned by the calling process. */
syscall_status_t channel_destroy(channel_id_t channel_id);

/* Dequeue the next capability call from an endpoint channel. */
syscall_status_t channel_recv(channel_id_t endpoint_id, struct cap_request* out_request, void* request_buffer,
                              size_t request_buffer_size, bool* out_received);

/* Complete a previously received capability call. */
syscall_status_t channel_reply(cap_call_id_t call_id, const void* response, size_t response_size,
                               syscall_status_t status);

/* Receive one pending channel lifecycle event without blocking. */
syscall_status_t channel_event_recv(channel_id_t channel, struct channel_event* out_event, bool* out_received);
