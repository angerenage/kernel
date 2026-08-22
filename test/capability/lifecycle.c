#include "test_support.h"

#define LIFECYCLE_MANY_OBJECTS 64u

static size_t lifecycle_callback_count;
static bool   lifecycle_callback_unpublish;

static syscall_result_t lifecycle_handler(const struct cap_request* request) {
	(void)request;
	return syscall_result_ok(0u);
}

static void lifecycle_callback(struct cap_object* object, enum cap_object_event event) {
	cr_assert_eq(event, CAP_OBJECT_EVENT_ZERO_GRANTS);
	lifecycle_callback_count++;
	if (lifecycle_callback_unpublish) cr_assert(cap_object_destroy(object));
}

static struct cap_object* create_userspace_object(struct channel* endpoint, uint64_t object_id,
                                                  cap_object_id_t* out_id) {
	*out_id = cap_object_create(object_id, endpoint, NULL);
	cr_assert_neq(*out_id, CAP_OBJECT_ID_INVALID);
	struct cap_object* object = cap_object_acquire(*out_id);
	cr_assert_not_null(object);
	return object;
}

static bool receive_zero_event(struct channel* endpoint, uint64_t* out_object_id) {
	for (;;) {
		struct cap_object* object = channel_dequeue_cap_event(endpoint);
		if (object == NULL) return false;
		if (cap_object_consume_zero_grants_event(object, out_object_id)) return true;
	}
}

Test(capability, grant_count_tracks_create_delegate_drop_revoke_and_cleanup) {
	struct cap_object* object;
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	cap_object_id_t    object_id;

	cap_test_setup();
	object_id = cap_object_create(0x200u, NULL, NULL);
	object    = cap_object_acquire(object_id);
	cr_assert_not_null(object);
	cr_assert_eq(cap_object_grant_count(object), 0u);
	root = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL));
	cr_assert_not_null(root);
	cr_assert_eq(cap_object_grant_count(object), 1u);
	child      = cap_lookup(cap_delegate_create(root, 11u, CAP_READ | CAP_DELEGATE, false));
	grandchild = cap_lookup(cap_delegate_create(root, 12u, CAP_READ, true));
	cr_assert_not_null(child);
	cr_assert_not_null(grandchild);
	cr_assert_eq(cap_object_grant_count(object), 3u);
	cr_assert(cap_drop(root));
	cr_assert_eq(cap_object_grant_count(object), 2u, "dropping a root must preserve and count spliced descendants");
	cr_assert_null(child->parent);
	cr_assert_null(grandchild->parent);
	cr_assert(cap_remove_rights(child, CAP_READ));
	cr_assert_eq(cap_object_grant_count(object), 2u, "rights-only revoke must not change grant count");
	cap_drop_for_process(11u);
	cr_assert_eq(cap_object_grant_count(object), 1u);
	cr_assert(cap_destroy(grandchild));
	cr_assert_eq(cap_object_grant_count(object), 0u);
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
}

Test(capability, derived_grants_count_against_their_own_object) {
	struct cap_object* first;
	struct cap_object* second;
	struct capability* parent;
	struct capability* derived;
	cap_object_id_t    first_id;
	cap_object_id_t    second_id;

	cap_test_setup();
	first_id  = cap_object_create(0x201u, NULL, NULL);
	second_id = cap_object_create(0x202u, NULL, NULL);
	first     = cap_object_acquire(first_id);
	second    = cap_object_acquire(second_id);
	parent    = cap_lookup(cap_create(first_id, 20u, CAP_READ | CAP_DELEGATE, NULL));
	derived   = cap_lookup(cap_create(second_id, 21u, CAP_READ, parent));
	cr_assert_not_null(derived);
	cr_assert_eq(cap_object_grant_count(first), 1u);
	cr_assert_eq(cap_object_grant_count(second), 1u);
	cr_assert(cap_destroy(parent));
	cr_assert_eq(cap_object_grant_count(first), 0u);
	cr_assert_eq(cap_object_grant_count(second), 0u);
	cr_assert(cap_object_destroy(first));
	cr_assert(cap_object_destroy(second));
	cap_object_release(first);
	cap_object_release(second);
}

Test(capability, zero_grants_is_once_per_epoch_and_stale_events_are_suppressed) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* grant;
	cap_object_id_t    object_id;
	uint64_t           delivered_id = 0u;

	cap_test_setup();
	endpoint = channel_create(30u);
	object   = create_userspace_object(endpoint, 0x203u, &object_id);
	grant    = cap_lookup(cap_create(object_id, 31u, CAP_READ, NULL));
	cr_assert(cap_destroy(grant));
	cr_assert(receive_zero_event(endpoint, &delivered_id));
	cr_assert_eq(delivered_id, 0x203u);
	cr_assert_not(receive_zero_event(endpoint, NULL));

	grant = cap_lookup(cap_create(object_id, 31u, CAP_READ, NULL));
	cr_assert(cap_destroy(grant));
	grant = cap_lookup(cap_create(object_id, 31u, CAP_READ, NULL));
	cr_assert_not(receive_zero_event(endpoint, NULL), "regrant must make a queued event stale");
	cr_assert(cap_destroy(grant));
	cr_assert(receive_zero_event(endpoint, &delivered_id));
	cr_assert_eq(delivered_id, 0x203u);
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 30u), CHANNEL_OK);
}

Test(capability, lifecycle_queue_is_lossless_beyond_request_queue_depth) {
	struct channel*    endpoint;
	struct cap_object* objects[LIFECYCLE_MANY_OBJECTS];
	cap_object_id_t    object_ids[LIFECYCLE_MANY_OBJECTS];
	bool               seen[LIFECYCLE_MANY_OBJECTS] = {0};

	cap_test_setup();
	endpoint = channel_create(40u);
	for (size_t i = 0u; i < LIFECYCLE_MANY_OBJECTS; i++) {
		objects[i]               = create_userspace_object(endpoint, 0x300u + i, &object_ids[i]);
		struct capability* grant = cap_lookup(cap_create(object_ids[i], 41u, CAP_READ, NULL));
		cr_assert(cap_destroy(grant));
	}
	for (size_t i = 0u; i < LIFECYCLE_MANY_OBJECTS; i++) {
		uint64_t object_id;
		cr_assert(receive_zero_event(endpoint, &object_id));
		cr_assert_geq(object_id, 0x300u);
		cr_assert_lt(object_id, 0x300u + LIFECYCLE_MANY_OBJECTS);
		seen[object_id - 0x300u] = true;
	}
	cr_assert_not(receive_zero_event(endpoint, NULL));
	for (size_t i = 0u; i < LIFECYCLE_MANY_OBJECTS; i++) {
		cr_assert(seen[i]);
		cr_assert(cap_object_destroy(objects[i]));
		cap_object_release(objects[i]);
	}
	cr_assert_eq(channel_destroy(endpoint, 40u), CHANNEL_OK);
}

Test(capability, pending_call_defers_zero_grants_until_terminal_completion) {
	struct channel*          endpoint;
	struct cap_object*       object;
	struct capability*       grant;
	struct cap_pending_call* pending;
	cap_object_id_t          object_id;
	struct cap_object*       active = NULL;

	cap_test_setup();
	endpoint = channel_create(50u);
	object   = create_userspace_object(endpoint, 0x204u, &object_id);
	grant    = cap_acquire(cap_create(object_id, 51u, CAP_CALL, NULL));
	cr_assert_eq(cap_object_begin_call(51u, grant, &active, NULL), CAP_OK);
	pending = cap_pending_call_create(active, endpoint->id, 50u, 51u, 0u);
	cr_assert_not_null(pending);
	cr_assert_eq(cap_object_active_call_count(object), 1u);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cr_assert(cap_pending_call_fail(cap_pending_call_id(pending), SYSCALL_STATUS_UNAVAILABLE));
	cr_assert_eq(cap_object_active_call_count(object), 0u);
	cr_assert(receive_zero_event(endpoint, NULL));
	cap_pending_call_destroy(pending);
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 50u), CHANNEL_OK);
}

Test(capability, caller_and_channel_cancellation_end_active_calls_once) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* grant;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(60u);
	object   = create_userspace_object(endpoint, 0x205u, &object_id);
	grant    = cap_acquire(cap_create(object_id, 61u, CAP_CALL, NULL));
	for (size_t i = 0u; i < 2u; i++) {
		struct cap_object* active = NULL;
		cr_assert_eq(cap_object_begin_call(61u, grant, &active, NULL), CAP_OK);
		struct cap_pending_call* pending = cap_pending_call_create(active, endpoint->id, 60u, 61u, 0u);
		cr_assert_not_null(pending);
		if (i == 0u) cap_pending_call_cancel_caller(61u);
		else cap_pending_call_cancel_channel(endpoint->id);
		cr_assert_eq(cap_object_active_call_count(object), 0u);
		cap_pending_call_destroy(pending);
	}
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 60u), CHANNEL_OK);
}

Test(capability, explicit_unpublish_invalidates_grants_suppresses_events_and_allows_active_calls_to_finish) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* grant;
	struct cap_object* active = NULL;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(70u);
	object   = create_userspace_object(endpoint, 0x206u, &object_id);
	grant    = cap_acquire(cap_create(object_id, 71u, CAP_CALL, NULL));
	cr_assert_eq(cap_object_begin_call(71u, grant, &active, NULL), CAP_OK);
	cr_assert(cap_object_unpublish(endpoint, 0x206u));
	cr_assert_eq(cap_is_valid(grant), CAP_NOT_FOUND);
	cr_assert_null(cap_object_acquire(object_id));
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cap_object_end_call(active);
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cap_release(grant);
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 70u), CHANNEL_OK);
}

Test(capability, explicit_unpublish_suppresses_an_already_queued_zero_event) {
	struct channel*    endpoint;
	struct cap_object* object;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(80u);
	object   = create_userspace_object(endpoint, 0x207u, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 81u, CAP_READ, NULL)));
	cr_assert(cap_object_unpublish(endpoint, 0x207u));
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 80u), CHANNEL_OK);
}

Test(capability, kernel_callback_runs_after_active_call_and_may_unpublish) {
	struct cap_object* object;
	struct capability* grant;
	struct cap_object* active  = NULL;
	bool               created = false;

	cap_test_setup();
	lifecycle_callback_count     = 0u;
	lifecycle_callback_unpublish = true;
	cap_object_id_t object_id =
		cap_object_create_kernel_lifecycle(0x208u, lifecycle_handler, NULL, NULL, lifecycle_callback, &created);
	cr_assert(created);
	object = cap_object_acquire(object_id);
	grant  = cap_acquire(cap_create(object_id, 90u, CAP_CALL, NULL));
	cr_assert_eq(cap_object_begin_call(90u, grant, &active, NULL), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert_eq(lifecycle_callback_count, 0u);
	cap_object_end_call(active);
	cr_assert_eq(lifecycle_callback_count, 1u);
	cr_assert_null(cap_object_acquire(object_id));
	cap_object_release(object);
}
