#include <core/message.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <string.h>

static size_t message_queue_next_index(size_t index) {
	return (index + 1u) % MESSAGE_QUEUE_DEPTH;
}

void message_queue_init(struct message_queue* queue) {
	if (queue == NULL) return;
	spinlock_init_class(&queue->lock, "message_queue", SPINLOCK_ORDER_PROCESS, SPINLOCK_FLAG_IRQSAVE);
	queue->head  = 0u;
	queue->tail  = 0u;
	queue->count = 0u;
	memset(queue->slots, 0, sizeof(queue->slots));
}

enum message_result message_queue_send(struct message_queue* queue, const void* data, size_t length) {
	struct irq_state state;
	struct message*  slot;

	if (queue == NULL) return MESSAGE_INVALID_ARGUMENTS;
	if (length > MESSAGE_MAX_SIZE) return MESSAGE_TOO_LARGE;
	if (length > 0u && data == NULL) return MESSAGE_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&queue->lock);
	if (queue->count >= MESSAGE_QUEUE_DEPTH) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return MESSAGE_QUEUE_FULL;
	}

	slot         = &queue->slots[queue->tail];
	slot->length = length;
	if (length > 0u) memcpy(slot->data, data, length);
	queue->tail = message_queue_next_index(queue->tail);
	queue->count++;
	spinlock_unlock_irqrestore(&queue->lock, state);
	return MESSAGE_OK;
}

enum message_result message_queue_receive(struct message_queue* queue, void* buffer, size_t buffer_size,
                                          size_t* out_length) {
	struct irq_state state;
	struct message*  slot;
	size_t           length;

	if (queue == NULL || out_length == NULL) return MESSAGE_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&queue->lock);
	if (queue->count == 0u) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return MESSAGE_NO_MESSAGE;
	}

	slot   = &queue->slots[queue->head];
	length = slot->length;
	if (length > buffer_size) {
		*out_length = length;
		spinlock_unlock_irqrestore(&queue->lock, state);
		return MESSAGE_TOO_LARGE;
	}
	if (length > 0u && buffer == NULL) {
		spinlock_unlock_irqrestore(&queue->lock, state);
		return MESSAGE_INVALID_ARGUMENTS;
	}

	if (length > 0u) memcpy(buffer, slot->data, length);
	queue->head = message_queue_next_index(queue->head);
	queue->count--;
	spinlock_unlock_irqrestore(&queue->lock, state);
	*out_length = length;
	return MESSAGE_OK;
}
