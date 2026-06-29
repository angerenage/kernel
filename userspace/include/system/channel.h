#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <stdbool.h>
#include <stddef.h>

/* Create a new endpoint channel owned by the calling process. */
bool channel_create(channel_id_t* out_id);

/* Destroy a channel owned by the calling process. */
bool channel_destroy(channel_id_t channel_id);

/* Dequeue the next capability call from an endpoint channel. */
bool channel_recv(channel_id_t endpoint_id, struct cap_request* out_request, void* request_buffer,
                  size_t request_buffer_size);

/* Complete a previously received capability call. */
bool channel_reply(cap_call_id_t call_id, const void* response, size_t response_size, bool success);
