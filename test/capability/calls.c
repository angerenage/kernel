#include <libc/stdlib.h>

#include "test_support.h"

Test(capability, pending_call_completes_with_bounded_response) {
	struct cap_pending_call* call;
	struct cap_pending_reply reply;
	struct channel* endpoint;
	syscall_result_t result;
	const uint32_t expected = 0x12345678u;

	cap_test_setup();
	endpoint = channel_create(42u, false);
	cr_assert_not_null(endpoint);
	call = cap_pending_call_create(NULL, endpoint, 42u, 12u, sizeof(expected));
	cr_assert_not_null(call);
	cr_assert_neq(cap_pending_call_id(call), CAP_CALL_ID_INVALID);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 99u, sizeof(expected), &reply),
			CAP_PENDING_REPLY_NOT_OWNER);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, sizeof(expected) + 1u, &reply),
			CAP_PENDING_REPLY_TOO_LARGE);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, sizeof(expected), &reply),
			CAP_PENDING_REPLY_OK);
	cap_pending_call_finish_reply(&reply, SYSCALL_STATUS_OK, sizeof(expected));

	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(expected));
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, 0u, &reply),
			CAP_PENDING_REPLY_ALREADY_COMPLETED);
	cap_pending_call_destroy(call);
	cr_assert_eq(channel_destroy(endpoint, 42u), CHANNEL_OK);
}

Test(capability, pending_call_can_complete_without_response_payload) {
	struct cap_pending_call* call;
	struct channel* endpoint;
	syscall_result_t result;

	cap_test_setup();
	endpoint = channel_create(43u, false);
	cr_assert_not_null(endpoint);
	call = cap_pending_call_create(NULL, endpoint, 43u, 13u, 0u);
	cr_assert_not_null(call);
	cr_assert(cap_pending_call_fail(cap_pending_call_id(call), SYSCALL_STATUS_UNAVAILABLE));
	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cap_pending_call_destroy(call);
	cr_assert_eq(channel_destroy(endpoint, 43u), CHANNEL_OK);
}

Test(capability, closing_channel_cancels_pending_calls) {
	struct cap_pending_call* call;
	struct channel* endpoint;
	syscall_result_t result;

	cap_test_setup();
	endpoint = channel_create(44u, false);
	cr_assert_not_null(endpoint);
	call = cap_pending_call_create(NULL, endpoint, 44u, 14u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_channel(endpoint);
	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cap_pending_call_destroy(call);
	cr_assert_eq(channel_destroy(endpoint, 44u), CHANNEL_OK);
}

Test(capability, terminating_caller_can_cancel_pending_calls) {
	struct cap_pending_call* call;
	struct channel* endpoint;
	syscall_result_t result;

	cap_test_setup();
	endpoint = channel_create(45u, false);
	cr_assert_not_null(endpoint);
	call = cap_pending_call_create(NULL, endpoint, 45u, 15u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_caller(15u);
	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cap_pending_call_destroy(call);
	cr_assert_eq(channel_destroy(endpoint, 45u), CHANNEL_OK);
}

Test(capability, terminating_provider_cancels_requests_waiting_for_its_reply) {
	struct cap_pending_call* call;
	struct channel* endpoint;
	syscall_result_t result;

	cap_test_setup();
	endpoint = channel_create(46u, false);
	cr_assert_not_null(endpoint);
	call = cap_pending_call_create(NULL, endpoint, 46u, 16u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_provider(46u);
	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cap_pending_call_destroy(call);
	cr_assert_eq(channel_destroy(endpoint, 46u), CHANNEL_OK);
}

Test(capability, stale_queued_call_id_does_not_resolve_a_later_call) {
	struct cap_pending_call* first;
	struct cap_pending_call* second;
	struct cap_pending_request received;
	struct cap_request queued;
	struct channel* channel;
	uint8_t* first_request;
	uint8_t* second_request;
	cap_call_id_t stale_id;

	cap_test_setup();
	channel = channel_create(47u, false);
	cr_assert_not_null(channel);

	first_request = malloc(1u);
	cr_assert_not_null(first_request);
	*first_request = 0x11u;
	first = cap_pending_call_create(NULL, channel, 47u, 17u, 0u);
	cr_assert_not_null(first);
	cr_assert(cap_pending_call_attach_request(first, first_request, 1u));
	stale_id = cap_pending_call_id(first);
	queued = (struct cap_request){.call_id = stale_id};
	cr_assert(channel_enqueue_cap_request(channel, &queued));
	cap_pending_call_destroy(first);

	second_request = malloc(1u);
	cr_assert_not_null(second_request);
	*second_request = 0x22u;
	second = cap_pending_call_create(NULL, channel, 47u, 18u, 0u);
	cr_assert_not_null(second);
	cr_assert(cap_pending_call_attach_request(second, second_request, 1u));
	cr_assert_eq((uint16_t)cap_pending_call_id(second),
			(uint16_t)stale_id,
			"the regression must reuse the removed pending-call table slot");
	cr_assert_neq(cap_pending_call_id(second), stale_id, "a removed pending-call ID must never be reused");
	queued = (struct cap_request){.call_id = cap_pending_call_id(second)};
	cr_assert(channel_enqueue_cap_request(channel, &queued));

	cr_assert(ring_buffer_dequeue(&channel->cap_queue, &queued));
	cr_assert_eq(queued.call_id, stale_id);
	cr_assert_not(cap_pending_call_prepare_receive(queued.call_id, 47u, &received),
			"a stale queue entry must not resolve to a later pending call");
	cr_assert(ring_buffer_dequeue(&channel->cap_queue, &queued));
	cr_assert_eq(queued.call_id, cap_pending_call_id(second));
	cr_assert(cap_pending_call_prepare_receive(queued.call_id, 47u, &received));
	cr_assert_eq(received.request_size, 1u);
	cr_assert_eq(*(const uint8_t*)received.request, 0x22u);
	cap_pending_call_finish_receive(&received);

	cap_pending_call_destroy(second);
	cr_assert_eq(channel_destroy(channel, 47u), CHANNEL_OK);
}
