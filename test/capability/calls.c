#include "test_support.h"

Test(capability, pending_call_completes_with_bounded_response) {
	struct cap_pending_call* call;
	struct cap_pending_reply reply;
	syscall_result_t         result;
	const void*              response;
	const uint32_t           expected = 0x12345678u;

	cap_test_setup();
	call = cap_pending_call_create(NULL, 7u, 42u, 12u, sizeof(expected));
	cr_assert_not_null(call);
	cr_assert_neq(cap_pending_call_id(call), CAP_CALL_ID_INVALID);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 99u, sizeof(expected), &reply),
	             CAP_PENDING_REPLY_NOT_OWNER);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, sizeof(expected) + 1u, &reply),
	             CAP_PENDING_REPLY_TOO_LARGE);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, sizeof(expected), &reply),
	             CAP_PENDING_REPLY_OK);
	memcpy(reply.response, &expected, sizeof(expected));
	cap_pending_call_finish_reply(&reply, SYSCALL_STATUS_OK, sizeof(expected));

	cap_pending_call_wait(call, &result, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(expected));
	cr_assert_not_null(response);
	cr_assert_eq(*(const uint32_t*)response, expected);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(call), 42u, 0u, &reply),
	             CAP_PENDING_REPLY_ALREADY_COMPLETED);
	cap_pending_call_destroy(call);
}

Test(capability, pending_call_can_complete_without_response_payload) {
	struct cap_pending_call* call;
	syscall_result_t         result;
	const void*              response;

	cap_test_setup();
	call = cap_pending_call_create(NULL, 8u, 43u, 13u, 0u);
	cr_assert_not_null(call);
	cr_assert(cap_pending_call_fail(cap_pending_call_id(call), SYSCALL_STATUS_UNAVAILABLE));
	cap_pending_call_wait(call, &result, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cr_assert_null(response);
	cap_pending_call_destroy(call);
}

Test(capability, closing_channel_cancels_pending_calls) {
	struct cap_pending_call* call;
	syscall_result_t         result;

	cap_test_setup();
	call = cap_pending_call_create(NULL, 11u, 44u, 14u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_channel(11u);
	cap_pending_call_wait(call, &result, NULL);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cap_pending_call_destroy(call);
}

Test(capability, terminating_caller_can_cancel_pending_calls) {
	struct cap_pending_call* call;
	syscall_result_t         result;

	cap_test_setup();
	call = cap_pending_call_create(NULL, 12u, 45u, 15u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_caller(15u);
	cap_pending_call_wait(call, &result, NULL);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cap_pending_call_destroy(call);
}

Test(capability, terminating_provider_cancels_requests_waiting_for_its_reply) {
	struct cap_pending_call* call;
	syscall_result_t         result;

	cap_test_setup();
	call = cap_pending_call_create(NULL, 13u, 46u, 16u, 0u);
	cr_assert_not_null(call);
	cap_pending_call_cancel_provider(46u);
	cap_pending_call_wait(call, &result, NULL);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cap_pending_call_destroy(call);
}
