#include <core/message.h>
#include <core/ring_buffer.h>
#include <stdbool.h>
#include <string.h>

bool message_queue_init(struct ring_buffer* rb) {
	if (rb == NULL) return false;
	return ring_buffer_init(rb, "message_queue", SPINLOCK_ORDER_PROCESS, MESSAGE_QUEUE_DEPTH, sizeof(struct message));
}

enum message_result message_queue_send(struct ring_buffer* rb, process_id_t sender_pid, const void* data,
                                       size_t length) {
	struct message msg;

	if (rb == NULL) return MESSAGE_INVALID_ARGUMENTS;
	if (length > MESSAGE_MAX_SIZE) return MESSAGE_TOO_LARGE;
	if (length > 0u && data == NULL) return MESSAGE_INVALID_ARGUMENTS;

	msg.sender_pid = sender_pid;
	msg.length     = length;
	if (length > 0u) memcpy(msg.data, data, length);

	if (!ring_buffer_enqueue(rb, &msg)) return MESSAGE_QUEUE_FULL;
	return MESSAGE_OK;
}

enum message_result message_queue_receive(struct ring_buffer* rb, void* buffer, size_t buffer_size, size_t* out_length,
                                          process_id_t* out_sender_pid) {
	struct ring_buffer_front front;
	const struct message*    msg;

	if (rb == NULL || out_length == NULL || out_sender_pid == NULL) return MESSAGE_INVALID_ARGUMENTS;

	if (!ring_buffer_front_acquire(rb, &front)) return MESSAGE_NO_MESSAGE;
	msg = front.element;

	if (msg->length > buffer_size) {
		*out_length = msg->length;
		ring_buffer_front_release(&front, false);
		return MESSAGE_TOO_LARGE;
	}
	if (msg->length > 0u && buffer == NULL) {
		ring_buffer_front_release(&front, false);
		return MESSAGE_INVALID_ARGUMENTS;
	}

	if (msg->length > 0u) memcpy(buffer, msg->data, msg->length);
	*out_length     = msg->length;
	*out_sender_pid = msg->sender_pid;
	ring_buffer_front_release(&front, true);
	return MESSAGE_OK;
}
