#pragma once

#include <base/message.h>
#include <core/spinlock.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed-size message payload stored in a queue slot. */
struct message {
	size_t  length;
	uint8_t data[MESSAGE_MAX_SIZE];
};

/* Per-process FIFO queue for fixed-size messages. */
struct message_queue {
	struct spinlock lock;
	size_t          head;
	size_t          tail;
	size_t          count;
	struct message  slots[MESSAGE_QUEUE_DEPTH];
};

/* Initialize an empty message queue. */
void message_queue_init(struct message_queue* queue);

/* Enqueue a message payload. */
enum message_result message_queue_send(struct message_queue* queue, const void* data, size_t length);

/* Dequeue the next message payload into the caller buffer. */
enum message_result message_queue_receive(struct message_queue* queue, void* buffer, size_t* out_length);
