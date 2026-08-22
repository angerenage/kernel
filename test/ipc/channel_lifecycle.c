#include "test_support.h"

Test(channel, create_rejects_invalid_arguments) {
	struct channel* ch;

	ipc_test_init_heap();
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

	ipc_test_init_heap();
	ch = channel_create(1u);
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
	ch = channel_create(3u);
	cr_assert_not_null(ch);
	object_id = cap_object_create(77u, ch, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);

	cr_assert_eq(channel_destroy(ch, 3u), CHANNEL_OK);
	cr_assert_null(cap_object_acquire(object_id));
}

Test(channel, destroy_unpublishes_id_before_last_retained_reference_is_released) {
	struct channel*    channel;
	struct channel*    held;
	struct cap_request request;
	channel_id_t       id;

	ipc_test_init_heap();
	capability_init();

	channel = channel_create(42u);
	cr_assert_not_null(channel);
	id   = channel->id;
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
	struct channel*          channel;
	struct cap_pending_call* call;
	syscall_result_t         result;

	ipc_test_init_heap();
	capability_init();

	channel = channel_create(77u);
	cr_assert_not_null(channel);
	call = cap_pending_call_create(NULL, channel->id, 77u, 55u, 0u);
	cr_assert_not_null(call);

	cr_assert_eq(channel_destroy(channel, 77u), CHANNEL_OK);
	cap_pending_call_wait(call, &result, NULL);
	cr_assert_eq(result.status,
	             SYSCALL_STATUS_UNAVAILABLE,
	             "closing the endpoint must wake callers waiting for a provider reply");

	cap_pending_call_destroy(call);
}

Test(channel, process_channel_state_deinit_unpublishes_every_owned_channel) {
	struct process_channel_state state;
	struct channel*              channels[8];
	channel_id_t                 ids[8];

	ipc_test_init_heap();
	capability_init();
	process_channel_state_init(&state);

	for (size_t i = 0u; i < 8u; i++) {
		channels[i] = channel_create(100u);
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
