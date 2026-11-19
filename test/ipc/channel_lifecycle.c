#include "test_support.h"

Test(channel, create_rejects_invalid_arguments) {
	struct channel* ch;

	ipc_test_init_heap();
	ch = channel_create(PROCESS_PID_INVALID, false);
	cr_assert_null(ch, "channel_create should reject invalid owner PID");

	ch = channel_create(1u, false);
	cr_assert_not_null(ch, "channel_create should succeed with valid owner PID");

	if (ch != NULL) {
		channel_destroy(ch, 1u);
	}
}

Test(channel, destroy_rejects_non_owner) {
	struct channel* ch;
	enum channel_result result;

	ipc_test_init_heap();
	ch = channel_create(1u, false);
	cr_assert_not_null(ch);

	result = channel_destroy(ch, 2u);
	cr_assert_eq(result, CHANNEL_NOT_OWNER, "channel_destroy should reject non-owner");

	result = channel_destroy(ch, 1u);
	cr_assert_eq(result, CHANNEL_OK, "channel_destroy should succeed for owner");
}

Test(channel, destroy_unpublishes_endpoint_objects) {
	struct channel* ch;
	cap_object_id_t object_id;

	ipc_test_init_heap();
	capability_init();
	ch = channel_create(3u, false);
	cr_assert_not_null(ch);
	object_id = cap_object_create(77u, ch, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);

	cr_assert_eq(channel_destroy(ch, 3u), CHANNEL_OK);
	cr_assert_null(cap_object_acquire(object_id));
}

Test(channel, destroy_unpublishes_id_before_last_retained_reference_is_released) {
	struct channel* channel;
	struct channel* held;
	struct cap_request request;
	channel_id_t id;

	ipc_test_init_heap();
	capability_init();

	channel = channel_create(42u, false);
	cr_assert_not_null(channel);
	id = channel->id;
	held = channel_acquire(id);
	cr_assert_eq(held, channel);

	memset(&request, 0, sizeof(request));
	cr_assert_eq(channel_destroy(channel, 42u), CHANNEL_OK);

	cr_assert_null(channel_acquire(id), "destroyed channel must reject new acquisitions immediately");
	cr_assert(held->closing, "retained descriptor must observe channel closure");
	cr_assert_not(channel_enqueue_cap_request(held, &request),
			"closure must reject new requests while an old reference keeps the descriptor alive");

	channel_release(held);
}

Test(channel, destroy_completes_pending_calls_as_unavailable) {
	struct channel* channel;
	struct cap_pending_call* call;
	syscall_result_t result;

	ipc_test_init_heap();
	capability_init();

	channel = channel_create(77u, false);
	cr_assert_not_null(channel);
	call = cap_pending_call_create(NULL, channel, 77u, 55u, 0u);
	cr_assert_not_null(call);

	cr_assert_eq(channel_destroy(channel, 77u), CHANNEL_OK);
	cap_pending_call_wait(call, &result);
	cr_assert_eq(result.status,
			SYSCALL_STATUS_UNAVAILABLE,
			"closing the endpoint must wake callers waiting for a provider reply");

	cap_pending_call_destroy(call);
}

Test(channel, late_old_instance_cancellation_does_not_cancel_reused_id) {
	struct cap_pending_call* old_call;
	struct cap_pending_call* replacement_call;
	struct cap_pending_reply reply;
	struct channel* old_channel;
	struct channel* replacement;
	syscall_result_t result;
	channel_id_t reused_id;

	ipc_test_init_heap();
	capability_init();

	old_channel = channel_create(78u, false);
	cr_assert_not_null(old_channel);
	reused_id = old_channel->id;
	cr_assert(channel_retain(old_channel));
	old_call = cap_pending_call_create(NULL, old_channel, 78u, 56u, 0u);
	cr_assert_not_null(old_call);

	cr_assert_eq(channel_destroy(old_channel, 78u), CHANNEL_OK);
	cr_assert(cap_pending_call_wait(old_call, &result));
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cap_pending_call_destroy(old_call);

	replacement = channel_create(78u, false);
	cr_assert_not_null(replacement);
	cr_assert_eq(replacement->id, reused_id, "the regression must reuse the removed channel ID");
	replacement_call = cap_pending_call_create(NULL, replacement, 78u, 57u, 0u);
	cr_assert_not_null(replacement_call);

	cap_pending_call_cancel_channel(old_channel);
	cr_assert_eq(cap_pending_call_prepare_reply(cap_pending_call_id(replacement_call), 78u, 0u, &reply),
			CAP_PENDING_REPLY_OK,
			"late teardown from the old channel must not claim the replacement call");
	cap_pending_call_finish_reply(&reply, SYSCALL_STATUS_OK, 0u);
	cr_assert(cap_pending_call_wait(replacement_call, &result));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	channel_release(old_channel);
	cap_pending_call_destroy(replacement_call);
	cr_assert_eq(channel_destroy(replacement, 78u), CHANNEL_OK);
}

Test(channel, process_channel_state_deinit_unpublishes_every_owned_channel) {
	struct process_channel_state state;
	struct channel* channels[8];
	channel_id_t ids[8];

	ipc_test_init_heap();
	capability_init();
	process_channel_state_init(&state);

	for (size_t i = 0u; i < 8u; i++) {
		channels[i] = channel_create(100u, false);
		cr_assert_not_null(channels[i]);
		ids[i] = channels[i]->id;
		cr_assert(process_channel_state_add(&state, channels[i]));
	}
	cr_assert_eq(state.count, 8u);

	process_channel_state_deinit(&state);
	cr_assert_eq(state.count, 0u);

	for (size_t i = 0u; i < 8u; i++) {
		cr_assert_null(
				channel_acquire(ids[i]), "channel %zu remained globally published after process channel teardown", i);
	}
}
