#pragma once

#include <base/message.h>
#include <base/process.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum message_recv_status {
	MESSAGE_RECV_OK = 0,
	MESSAGE_RECV_EMPTY,
	MESSAGE_RECV_TOO_SMALL,
	MESSAGE_RECV_FAILED,
} message_recv_status_t;

/* Send a message payload to the process identified by pid. */
bool message_send(process_id_t pid, const void* buffer, size_t length);

/* Receive the next message. */
message_recv_status_t message_recv(void* buffer, size_t buffer_size, size_t* out_length, process_id_t* out_sender_pid,
                                   size_t* out_required_size);
