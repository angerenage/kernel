#include <base/message.h>
#include <core/message.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

Test(message_queue, starts_empty_and_reports_no_message) {
	struct message_queue queue;
	size_t               length = 123u;
	process_id_t         sender = 99u;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_receive(&queue, NULL, 0u, &length, &sender), MESSAGE_NO_MESSAGE);
	cr_assert_eq(length, 123u, "receive should not update length when empty");
	cr_assert_eq(sender, 99u, "receive should not update sender when empty");
}

Test(message_queue, preserves_fifo_order) {
	struct message_queue queue;
	uint8_t              buffer[MESSAGE_MAX_SIZE];
	size_t               length;
	process_id_t         sender        = PROCESS_PID_INVALID;
	const char           first[]       = "first";
	const char           second[]      = "second";
	process_id_t         sender_first  = 101u;
	process_id_t         sender_second = 202u;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(&queue, sender_first, first, sizeof(first)), MESSAGE_OK);
	cr_assert_eq(message_queue_send(&queue, sender_second, second, sizeof(second)), MESSAGE_OK);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(first));
	cr_assert_eq(memcmp(buffer, first, sizeof(first)), 0);
	cr_assert_eq(sender, sender_first);

	length = 0u;
	sender = PROCESS_PID_INVALID;
	cr_assert_eq(message_queue_receive(&queue, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(second));
	cr_assert_eq(memcmp(buffer, second, sizeof(second)), 0);
	cr_assert_eq(sender, sender_second);
}

Test(message_queue, rejects_invalid_arguments) {
	struct message_queue queue;
	size_t               length;
	uint8_t              buffer[MESSAGE_MAX_SIZE];
	const char           payload[] = "payload";
	process_id_t         sender    = PROCESS_PID_INVALID;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(NULL, sender, payload, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_send(&queue, sender, NULL, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(NULL, buffer, sizeof(buffer), &length, &sender), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(&queue, buffer, sizeof(buffer), NULL, &sender), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(&queue, buffer, sizeof(buffer), &length, NULL), MESSAGE_INVALID_ARGUMENTS);

	cr_assert_eq(message_queue_send(&queue, sender, payload, sizeof(payload)), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&queue, NULL, 0u, &length, &sender), MESSAGE_TOO_LARGE);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(message_queue_receive(&queue, NULL, sizeof(payload), &length, &sender), MESSAGE_INVALID_ARGUMENTS);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(payload));
}

Test(message_queue, accepts_empty_payloads) {
	struct message_queue queue;
	size_t               length = 1u;
	process_id_t         sender = PROCESS_PID_INVALID;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(&queue, sender, NULL, 0u), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&queue, NULL, 0u, &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, 0u);
}

Test(message_queue, reports_required_size_when_buffer_too_small) {
	struct message_queue queue;
	uint8_t              buffer_small[4];
	size_t               length    = 0u;
	const char           payload[] = "message";
	uint8_t              buffer_full[sizeof(payload)];
	process_id_t         sender          = 77u;
	process_id_t         received_sender = PROCESS_PID_INVALID;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(&queue, sender, payload, sizeof(payload)), MESSAGE_OK);

	cr_assert_eq(message_queue_receive(&queue, buffer_small, sizeof(buffer_small), &length, &received_sender),
	             MESSAGE_TOO_LARGE);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(received_sender, PROCESS_PID_INVALID);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer_full, sizeof(buffer_full), &length, &received_sender),
	             MESSAGE_OK);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(memcmp(buffer_full, payload, sizeof(payload)), 0);
	cr_assert_eq(received_sender, sender);
}

Test(message_queue, rejects_oversize_payloads_and_queue_full) {
	struct message_queue queue;
	uint8_t              buffer[MESSAGE_MAX_SIZE + 1u];
	uint8_t              byte   = 0x5a;
	process_id_t         sender = 1u;

	message_queue_init(&queue);
	memset(buffer, 0, sizeof(buffer));
	cr_assert_eq(message_queue_send(&queue, sender, buffer, MESSAGE_MAX_SIZE + 1u), MESSAGE_TOO_LARGE);

	for (size_t i = 0u; i < MESSAGE_QUEUE_DEPTH; i++) {
		cr_assert_eq(message_queue_send(&queue, sender, &byte, sizeof(byte)), MESSAGE_OK);
	}
	cr_assert_eq(message_queue_send(&queue, sender, &byte, sizeof(byte)), MESSAGE_QUEUE_FULL);
}
