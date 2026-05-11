#include <base/message.h>
#include <core/message.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

Test(message_queue, starts_empty_and_reports_no_message) {
	struct message_queue queue;
	size_t               length = 123u;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_receive(&queue, NULL, &length), MESSAGE_NO_MESSAGE);
	cr_assert_eq(length, 123u, "receive should not update length when empty");
}

Test(message_queue, preserves_fifo_order) {
	struct message_queue queue;
	uint8_t              buffer[MESSAGE_MAX_SIZE];
	size_t               length;
	const char           first[]  = "first";
	const char           second[] = "second";

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(&queue, first, sizeof(first)), MESSAGE_OK);
	cr_assert_eq(message_queue_send(&queue, second, sizeof(second)), MESSAGE_OK);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer, &length), MESSAGE_OK);
	cr_assert_eq(length, sizeof(first));
	cr_assert_eq(memcmp(buffer, first, sizeof(first)), 0);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer, &length), MESSAGE_OK);
	cr_assert_eq(length, sizeof(second));
	cr_assert_eq(memcmp(buffer, second, sizeof(second)), 0);
}

Test(message_queue, rejects_invalid_arguments) {
	struct message_queue queue;
	size_t               length;
	uint8_t              buffer[MESSAGE_MAX_SIZE];
	const char           payload[] = "payload";

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(NULL, payload, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_send(&queue, NULL, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(NULL, buffer, &length), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(&queue, buffer, NULL), MESSAGE_INVALID_ARGUMENTS);

	cr_assert_eq(message_queue_send(&queue, payload, sizeof(payload)), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&queue, NULL, &length), MESSAGE_INVALID_ARGUMENTS);

	length = 0u;
	cr_assert_eq(message_queue_receive(&queue, buffer, &length), MESSAGE_OK);
	cr_assert_eq(length, sizeof(payload));
}

Test(message_queue, accepts_empty_payloads) {
	struct message_queue queue;
	size_t               length = 1u;

	message_queue_init(&queue);
	cr_assert_eq(message_queue_send(&queue, NULL, 0u), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&queue, NULL, &length), MESSAGE_OK);
	cr_assert_eq(length, 0u);
}

Test(message_queue, rejects_oversize_payloads_and_queue_full) {
	struct message_queue queue;
	uint8_t              buffer[MESSAGE_MAX_SIZE + 1u];
	uint8_t              byte = 0x5a;

	message_queue_init(&queue);
	memset(buffer, 0, sizeof(buffer));
	cr_assert_eq(message_queue_send(&queue, buffer, MESSAGE_MAX_SIZE + 1u), MESSAGE_TOO_LARGE);

	for (size_t i = 0u; i < MESSAGE_QUEUE_DEPTH; i++) {
		cr_assert_eq(message_queue_send(&queue, &byte, sizeof(byte)), MESSAGE_OK);
	}
	cr_assert_eq(message_queue_send(&queue, &byte, sizeof(byte)), MESSAGE_QUEUE_FULL);
}
