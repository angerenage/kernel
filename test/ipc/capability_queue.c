#include <core/signal.h>

#include "test_support.h"

Test(channel, cap_queue_init_is_empty) {
	struct channel*    ch;
	struct cap_request req;

	ipc_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, &req), "new channel cap_queue should be empty");

	channel_destroy(ch, 1u);
}
Test(channel, cap_queue_send_and_recv) {
	struct channel*    ch;
	struct cap_request req;
	struct cap_request out;

	ipc_test_init_heap();
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

	ipc_test_init_heap();
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

	ipc_test_init_heap();
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

	ipc_test_init_heap();
	ch = channel_create(1u);
	cr_assert_not_null(ch);

	cr_assert_not(ring_buffer_enqueue(NULL, &req));
	cr_assert_not(ring_buffer_enqueue(&ch->cap_queue, NULL));
	cr_assert_not(ring_buffer_dequeue(NULL, &req));
	cr_assert_not(ring_buffer_dequeue(&ch->cap_queue, NULL));

	channel_destroy(ch, 1u);
}
