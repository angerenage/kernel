#include <base/cap.h>
#include <base/channel.h>
#include <base/heap.h>
#include <base/process.h>
#include <core/capability.h>
#include <core/channel.h>
#include <core/pmm.h>
#include <core/ring_buffer.h>
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

Test(channel, destroy_unpublishes_endpoint_objects) {
	struct channel*    ch;
	struct cap_object* object;
	cap_object_id_t    object_id;

	channel_test_init_heap();
	capability_init();
	ch = channel_create(3u);
	cr_assert_not_null(ch);
	object = cap_object_create(77u, ch);
	cr_assert_not_null(object);
	object_id = object->cap_object_id;

	cr_assert_eq(channel_destroy(ch, 3u), CHANNEL_OK);
	cr_assert_null(cap_object_acquire(object_id));
}

Test(channel, cap_queue_init_is_empty) {
	struct channel*    ch;
	struct cap_request req;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, &req), "new channel cap_queue should be empty");

	channel_destroy(ch, 1u);
}

Test(channel, cap_queue_send_and_recv) {
	struct channel*    ch;
	struct cap_request req;
	struct cap_request out;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	req.caller       = 5u;
	req.cap_id       = 42u;
	req.object_id    = 100u;
	req.rights       = CAP_READ;
	req.request      = NULL;
	req.request_size = 0u;

	cr_assert(ring_buffer_enqueue(&ch->cap_queue, &req), "enqueue should succeed");

	cr_assert(ring_buffer_dequeue(&ch->cap_queue, &out), "dequeue should succeed");
	cr_assert_eq(out.caller, 5u);
	cr_assert_eq(out.cap_id, 42u);
	cr_assert_eq(out.object_id, 100u);
	cr_assert_eq(out.rights, CAP_READ);
	cr_assert_eq(out.request_size, 0u);

	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, &out), "queue should be empty after dequeue");

	channel_destroy(ch, 1u);
}

Test(channel, cap_queue_fifo_order) {
	struct channel*    ch;
	struct cap_request req1;
	struct cap_request req2;
	struct cap_request out;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	req1.caller       = 1u;
	req1.cap_id       = 10u;
	req1.object_id    = 100u;
	req1.rights       = CAP_READ;
	req1.request      = NULL;
	req1.request_size = 0u;

	req2.caller       = 2u;
	req2.cap_id       = 20u;
	req2.object_id    = 200u;
	req2.rights       = CAP_WRITE;
	req2.request      = NULL;
	req2.request_size = 0u;

	cr_assert(ring_buffer_enqueue(&ch->cap_queue, &req1));
	cr_assert(ring_buffer_enqueue(&ch->cap_queue, &req2));

	cr_assert(ring_buffer_dequeue(&ch->cap_queue, &out));
	cr_assert_eq(out.caller, 1u);
	cr_assert_eq(out.cap_id, 10u);

	cr_assert(ring_buffer_dequeue(&ch->cap_queue, &out));
	cr_assert_eq(out.caller, 2u);
	cr_assert_eq(out.cap_id, 20u);

	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, &out));

	channel_destroy(ch, 1u);
}

Test(channel, cap_queue_full) {
	struct channel*    ch;
	struct cap_request req;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	memset(&req, 0, sizeof(req));
	for (size_t i = 0u; i < CAP_REQUEST_QUEUE_DEPTH; i++) {
		cr_assert(ring_buffer_enqueue(&ch->cap_queue, &req), "enqueue %zu should succeed", i);
	}

	cr_assert_not(ring_buffer_enqueue(&ch->cap_queue, &req), "enqueue should fail when queue is full");

	channel_destroy(ch, 1u);
}

Test(channel, cap_queue_rejects_null) {
	struct channel*    ch;
	struct cap_request req;

	channel_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	cr_assert_not(ring_buffer_enqueue(NULL, &req));
	cr_assert_not(ring_buffer_enqueue(&ch->cap_queue, NULL));
	cr_assert_not(ring_buffer_dequeue(NULL, &req));
	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, NULL));

	channel_destroy(ch, 1u);
}
