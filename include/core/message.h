#pragma once

#include <base/message.h>
#include <base/process.h>
#include <core/ring_buffer.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed-size message payload stored in a queue slot. */
struct message {
	process_id_t sender_pid;
	size_t       length;
	uint8_t      data[MESSAGE_MAX_SIZE];
};

/* Initialize a message ring buffer. */
bool message_queue_init(struct ring_buffer* rb);

/* Enqueue a message payload. */
enum message_result message_queue_send(struct ring_buffer* rb, process_id_t sender_pid, const void* data,
                                       size_t length);

/* Atomically validate and dequeue the next message, leaving it queued when the caller buffer is too small. */
enum message_result message_queue_receive(struct ring_buffer* rb, void* buffer, size_t buffer_size, size_t* out_length,
                                          process_id_t* out_sender_pid);
