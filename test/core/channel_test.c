#include <base/channel.h>
#include <base/heap.h>
#include <base/process.h>
#include <core/channel.h>
#include <core/message.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KiB(x) ((size_t)(x) * 1024u)
#define CHANNEL_TEST_HEAP_SIZE KiB(64)

static uint8_t channel_test_heap[CHANNEL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  channel_test_heap_offset;
static bool    channel_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&channel_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > CHANNEL_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&channel_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = channel_test_heap + offset;
			return true;
		}
	}
}

static void channel_test_init_heap(void) {
	if (channel_test_heap_initialized) return;

	channel_test_heap_offset = 0u;
	cr_assert(heap_init(), "heap_init failed");
	channel_test_heap_initialized = true;
}

Test(channel, create_rejects_invalid_arguments) {
	struct channel* ch;

	channel_test_init_heap();
	ch = channel_create(PROCESS_PID_INVALID);
	cr_assert_null(ch, "channel_create should reject invalid owner PID");

	ch = channel_create(1u);
	cr_assert_not_null(ch, "channel_create should succeed with valid owner PID");

	if (ch != NULL) {
		channel_destroy(ch, 1u);
	}
}

Test(channel, destroy_rejects_non_owner) {
	struct channel*     ch;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_destroy(ch, 2u);
	cr_assert_eq(result, CHANNEL_NOT_OWNER, "channel_destroy should reject non-owner");

	result = channel_destroy(ch, 1u);
	cr_assert_eq(result, CHANNEL_OK, "channel_destroy should succeed for owner");
}

Test(channel, send_rejects_invalid_arguments) {
	struct channel*     ch;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_send(NULL, 1u, "test", 4u);
	cr_assert_eq(result, CHANNEL_INVALID_ARGUMENTS, "channel_send should reject NULL channel");

	result = channel_send(ch, 1u, NULL, 4u);
	cr_assert_eq(result, CHANNEL_INVALID_ARGUMENTS, "channel_send should reject NULL data with non-zero length");

	result = channel_send(ch, 1u, "test", 4u);
	cr_assert_eq(result, CHANNEL_OK, "channel_send should succeed with valid arguments");

	channel_destroy(ch, 1u);
}

Test(channel, send_accepts_empty_payloads) {
	struct channel*     ch;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_send(ch, 1u, NULL, 0u);
	cr_assert_eq(result, CHANNEL_OK, "channel_send should accept NULL data with zero length");

	channel_destroy(ch, 1u);
}

Test(channel, recv_rejects_non_owner) {
	struct channel*     ch;
	struct message      msg;
	size_t              length;
	process_id_t        sender;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_recv(ch, 2u, &msg, sizeof(msg), &length, &sender);
	cr_assert_eq(result, CHANNEL_NOT_OWNER, "channel_recv should reject non-owner");

	result = channel_recv(ch, 1u, &msg, sizeof(msg), &length, &sender);
	cr_assert_eq(result, CHANNEL_NO_MESSAGE, "channel_recv should return NO_MESSAGE for owner on empty queue");

	channel_destroy(ch, 1u);
}

Test(channel, recv_accepts_empty_payloads) {
	struct channel*     ch;
	size_t              length;
	process_id_t        sender;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_send(ch, 100u, "hello", 5u);
	cr_assert_eq(result, CHANNEL_OK);

	result = channel_recv(ch, 1u, NULL, 0u, &length, &sender);
	cr_assert_eq(result,
	             CHANNEL_BUFFER_TOO_SMALL,
	             "channel_recv should report a too-small buffer with CHANNEL_BUFFER_TOO_SMALL");
	cr_assert_eq(length, 5u, "channel_recv should report required size");

	result = channel_recv(ch, 1u, NULL, 5u, &length, &sender);
	cr_assert_eq(result, CHANNEL_INVALID_ARGUMENTS, "channel_recv should reject NULL buffer");

	char buffer[64] = {0};
	result          = channel_recv(ch, 1u, buffer, sizeof(buffer), &length, &sender);
	cr_assert_eq(result, CHANNEL_OK, "channel_recv should succeed with valid buffer");
	cr_assert_eq(length, 5u, "channel_recv should report correct length");
	cr_assert_eq(memcmp(buffer, "hello", 5u), 0, "channel_recv should copy correct data");
	cr_assert_eq(sender, 100u, "channel_recv should report correct sender");

	channel_destroy(ch, 1u);
}

Test(channel, fifo_order_preserved) {
	struct channel*     ch;
	char                buffer[64];
	size_t              length;
	process_id_t        sender;
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_send(ch, 100u, "first", 5u);
	cr_assert_eq(result, CHANNEL_OK);

	result = channel_send(ch, 200u, "second", 6u);
	cr_assert_eq(result, CHANNEL_OK);

	result = channel_recv(ch, 1u, buffer, sizeof(buffer), &length, &sender);
	cr_assert_eq(result, CHANNEL_OK);
	cr_assert_eq(length, 5u);
	cr_assert_eq(memcmp(buffer, "first", 5u), 0);
	cr_assert_eq(sender, 100u);

	memset(buffer, 0, sizeof(buffer));
	result = channel_recv(ch, 1u, buffer, sizeof(buffer), &length, &sender);
	cr_assert_eq(result, CHANNEL_OK);
	cr_assert_eq(length, 6u);
	cr_assert_eq(memcmp(buffer, "second", 6u), 0);
	cr_assert_eq(sender, 200u);

	channel_destroy(ch, 1u);
}

Test(channel, handles_max_payload_limit) {
	struct channel*     ch;
	char                buffer[MESSAGE_MAX_SIZE + 1u];
	enum channel_result result;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	result = channel_send(ch, 1u, buffer, MESSAGE_MAX_SIZE + 1u);
	cr_assert_eq(result, CHANNEL_INVALID_ARGUMENTS, "channel_send should reject oversized messages");

	channel_destroy(ch, 1u);
}
