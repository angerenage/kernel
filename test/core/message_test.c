#include <base/heap.h>
#include <base/message.h>
#include <core/message.h>
#include <core/pmm.h>
#include <core/ring_buffer.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KiB(x) ((size_t)(x) * 1024u)
#define MESSAGE_TEST_HEAP_SIZE KiB(64)

static uint8_t message_test_heap[MESSAGE_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  message_test_heap_offset;
static bool    message_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&message_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > MESSAGE_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&message_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = message_test_heap + offset;
			return true;
		}
	}
}

static void message_test_init_heap(void) {
	if (message_test_heap_initialized) return;

	message_test_heap_offset = 0u;
	cr_assert(heap_init(), "heap_init failed");
	message_test_heap_initialized = true;
}

Test(message_queue, starts_empty_and_reports_no_message) {
	struct ring_buffer rb;
	size_t             length = 123u;
	process_id_t       sender = 99u;

	message_test_init_heap();
	message_queue_init(&rb);
	cr_assert_eq(message_queue_receive(&rb, NULL, 0u, &length, &sender), MESSAGE_NO_MESSAGE);
	cr_assert_eq(length, 123u, "receive should not update length when empty");
	cr_assert_eq(sender, 99u, "receive should not update sender when empty");
}

Test(message_queue, preserves_fifo_order) {
	struct ring_buffer rb;
	uint8_t            buffer[MESSAGE_MAX_SIZE];
	size_t             length;
	process_id_t       sender        = PROCESS_PID_INVALID;
	const char         first[]       = "first";
	const char         second[]      = "second";
	process_id_t       sender_first  = 101u;
	process_id_t       sender_second = 202u;

	message_test_init_heap();
	message_queue_init(&rb);
	cr_assert_eq(message_queue_send(&rb, sender_first, first, sizeof(first)), MESSAGE_OK);
	cr_assert_eq(message_queue_send(&rb, sender_second, second, sizeof(second)), MESSAGE_OK);

	length = 0u;
	cr_assert_eq(message_queue_receive(&rb, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(first));
	cr_assert_eq(memcmp(buffer, first, sizeof(first)), 0);
	cr_assert_eq(sender, sender_first);

	length = 0u;
	sender = PROCESS_PID_INVALID;
	cr_assert_eq(message_queue_receive(&rb, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(second));
	cr_assert_eq(memcmp(buffer, second, sizeof(second)), 0);
	cr_assert_eq(sender, sender_second);
}

Test(message_queue, rejects_invalid_arguments) {
	struct ring_buffer rb;
	size_t             length;
	uint8_t            buffer[MESSAGE_MAX_SIZE];
	const char         payload[] = "payload";
	process_id_t       sender    = PROCESS_PID_INVALID;

	message_test_init_heap();
	message_queue_init(&rb);
	cr_assert_eq(message_queue_send(NULL, sender, payload, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_send(&rb, sender, NULL, sizeof(payload)), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(NULL, buffer, sizeof(buffer), &length, &sender), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(&rb, buffer, sizeof(buffer), NULL, &sender), MESSAGE_INVALID_ARGUMENTS);
	cr_assert_eq(message_queue_receive(&rb, buffer, sizeof(buffer), &length, NULL), MESSAGE_INVALID_ARGUMENTS);

	cr_assert_eq(message_queue_send(&rb, sender, payload, sizeof(payload)), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&rb, NULL, 0u, &length, &sender), MESSAGE_TOO_LARGE);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(message_queue_receive(&rb, NULL, sizeof(payload), &length, &sender), MESSAGE_INVALID_ARGUMENTS);

	length = 0u;
	cr_assert_eq(message_queue_receive(&rb, buffer, sizeof(buffer), &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(payload));
}

Test(message_queue, accepts_empty_payloads) {
	struct ring_buffer rb;
	size_t             length = 1u;
	process_id_t       sender = PROCESS_PID_INVALID;

	message_test_init_heap();
	message_queue_init(&rb);
	cr_assert_eq(message_queue_send(&rb, sender, NULL, 0u), MESSAGE_OK);
	cr_assert_eq(message_queue_receive(&rb, NULL, 0u, &length, &sender), MESSAGE_OK);
	cr_assert_eq(length, 0u);
}

Test(message_queue, reports_required_size_when_buffer_too_small) {
	struct ring_buffer rb;
	uint8_t            buffer_small[4];
	size_t             length    = 0u;
	const char         payload[] = "message";
	uint8_t            buffer_full[sizeof(payload)];
	process_id_t       sender          = 77u;
	process_id_t       received_sender = PROCESS_PID_INVALID;

	message_test_init_heap();
	message_queue_init(&rb);
	cr_assert_eq(message_queue_send(&rb, sender, payload, sizeof(payload)), MESSAGE_OK);

	cr_assert_eq(message_queue_receive(&rb, buffer_small, sizeof(buffer_small), &length, &received_sender),
	             MESSAGE_TOO_LARGE);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(received_sender, PROCESS_PID_INVALID);

	length = 0u;
	cr_assert_eq(message_queue_receive(&rb, buffer_full, sizeof(buffer_full), &length, &received_sender), MESSAGE_OK);
	cr_assert_eq(length, sizeof(payload));
	cr_assert_eq(memcmp(buffer_full, payload, sizeof(payload)), 0);
	cr_assert_eq(received_sender, sender);
}

Test(message_queue, rejects_oversize_payloads_and_queue_full) {
	struct ring_buffer rb;
	uint8_t            buffer[MESSAGE_MAX_SIZE + 1u];
	uint8_t            byte   = 0x5a;
	process_id_t       sender = 1u;

	message_test_init_heap();
	message_queue_init(&rb);
	memset(buffer, 0, sizeof(buffer));
	cr_assert_eq(message_queue_send(&rb, sender, buffer, MESSAGE_MAX_SIZE + 1u), MESSAGE_TOO_LARGE);

	for (size_t i = 0u; i < MESSAGE_QUEUE_DEPTH; i++) {
		cr_assert_eq(message_queue_send(&rb, sender, &byte, sizeof(byte)), MESSAGE_OK);
	}
	cr_assert_eq(message_queue_send(&rb, sender, &byte, sizeof(byte)), MESSAGE_QUEUE_FULL);
}
