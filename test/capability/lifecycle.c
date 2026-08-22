#include <pthread.h>
#include <signal.h>

#include "test_support.h"

#define LIFECYCLE_MANY_OBJECTS 64u

static size_t lifecycle_callback_count;
static bool   lifecycle_callback_unpublish;
static size_t direct_use_destroy_count;

struct direct_use_race {
	cap_id_t           cap_id;
	process_id_t       caller;
	cap_rights_t       rights;
	struct capability* dropped_cap;
	struct cap_object* acquired_object;
	enum cap_result    result;
	size_t             ready;
	bool               start;
	bool               dropped;
};

static void direct_use_destroy(uint64_t object_id) {
	(void)object_id;
	direct_use_destroy_count++;
}

static void* direct_use_acquire_worker(void* argument) {
	struct direct_use_race* race = argument;
	(void)__atomic_add_fetch(&race->ready, 1u, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&race->start, __ATOMIC_ACQUIRE)) {
	}
	race->result = cap_object_acquire_for_use(race->caller, race->cap_id, race->rights, &race->acquired_object, NULL);
	return NULL;
}

static void* direct_use_drop_worker(void* argument) {
	struct direct_use_race* race = argument;
	(void)__atomic_add_fetch(&race->ready, 1u, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&race->start, __ATOMIC_ACQUIRE)) {
	}
	race->dropped = cap_drop(race->dropped_cap);
	return NULL;
}

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
		if (cap_object_prepare_zero_grants_event(object, out_object_id)) {
			return cap_object_commit_zero_grants_event(object);
		}
	}
}

Test(capability, lifecycle_state_fits_in_the_reduced_cap_object_layout) {
	cr_assert_eq(sizeof(struct cap_object), 96u);
	cr_assert_lt(sizeof(struct cap_object), 104u);
}

Test(capability, direct_use_linearizes_against_drop) {
	struct direct_use_race race = {.caller = 101u, .rights = CAP_READ, .result = CAP_INVALID_ARGUMENTS};
	struct cap_object*     published;
	cap_object_id_t        object_id;
	pthread_t              acquire_thread;
	pthread_t              drop_thread;

	cap_test_setup();
	object_id        = cap_object_create_kernel(0x301u, lifecycle_handler, NULL);
	published        = cap_object_acquire(object_id);
	race.cap_id      = cap_create(object_id, race.caller, CAP_READ, NULL);
	race.dropped_cap = cap_acquire(race.cap_id);
	cr_assert_not_null(published);
	cr_assert_not_null(race.dropped_cap);
	cr_assert_eq(pthread_create(&acquire_thread, NULL, direct_use_acquire_worker, &race), 0);
	cr_assert_eq(pthread_create(&drop_thread, NULL, direct_use_drop_worker, &race), 0);
	while (__atomic_load_n(&race.ready, __ATOMIC_ACQUIRE) != 2u) {
	}
	__atomic_store_n(&race.start, true, __ATOMIC_RELEASE);
	cr_assert_eq(pthread_join(acquire_thread, NULL), 0);
	cr_assert_eq(pthread_join(drop_thread, NULL), 0);
	cr_assert(race.dropped);
	cr_assert(race.result == CAP_OK || race.result == CAP_NOT_FOUND);
	if (race.result == CAP_OK) {
		cr_assert_eq(race.acquired_object->object_id, 0x301u);
		cap_object_release(race.acquired_object);
	}
	cap_release(race.dropped_cap);
	cr_assert(cap_object_destroy(published));
	cap_object_release(published);
}

Test(capability, direct_use_linearizes_against_authorization_topology_changes) {
	struct direct_use_race race = {.caller = 102u, .rights = CAP_READ, .result = CAP_INVALID_ARGUMENTS};
	struct cap_object*     published;
	cap_object_id_t        object_id;
	struct capability*     child;
	pthread_t              acquire_thread;
	pthread_t              drop_thread;

	cap_test_setup();
	object_id        = cap_object_create_kernel(0x302u, lifecycle_handler, NULL);
	published        = cap_object_acquire(object_id);
	race.dropped_cap = cap_acquire(cap_create(object_id, race.caller, CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(race.dropped_cap);
	race.cap_id = cap_delegate_create(race.dropped_cap, 103u, CAP_READ, false);
	child       = cap_acquire(race.cap_id);
	cr_assert_not_null(child);
	cr_assert_eq(pthread_create(&acquire_thread, NULL, direct_use_acquire_worker, &race), 0);
	cr_assert_eq(pthread_create(&drop_thread, NULL, direct_use_drop_worker, &race), 0);
	while (__atomic_load_n(&race.ready, __ATOMIC_ACQUIRE) != 2u) {
	}
	__atomic_store_n(&race.start, true, __ATOMIC_RELEASE);
	cr_assert_eq(pthread_join(acquire_thread, NULL), 0);
	cr_assert_eq(pthread_join(drop_thread, NULL), 0);
	cr_assert(race.dropped);
	cr_assert(race.result == CAP_OK || race.result == CAP_NOT_AUTHORIZED);
	if (race.result == CAP_OK) cap_object_release(race.acquired_object);
	cr_assert_eq(cap_object_acquire_for_use(race.caller, race.cap_id, CAP_READ, &race.acquired_object, NULL),
	             CAP_NOT_AUTHORIZED);
	cr_assert(cap_destroy(child));
	cap_release(child);
	cap_release(race.dropped_cap);
	cr_assert(cap_object_destroy(published));
	cap_object_release(published);
}

Test(capability, direct_use_checks_rights_and_retains_the_unpublished_object) {
	struct cap_object* acquired = NULL;
	struct cap_object* published;
	cap_object_id_t    object_id;
	cap_id_t           cap_id;
	cap_rights_t       rights = 0u;

	cap_test_setup();
	direct_use_destroy_count = 0u;
	object_id = cap_object_create_kernel_managed(0x303u, lifecycle_handler, NULL, direct_use_destroy, NULL);
	published = cap_object_acquire(object_id);
	cap_id    = cap_create(object_id, 104u, CAP_READ, NULL);
	cr_assert_eq(cap_object_acquire_for_use(104u, cap_id, CAP_WRITE, &acquired, &rights), CAP_RIGHTS_EXCEEDED);
	cr_assert_null(acquired);
	cr_assert_eq(rights, 0u);
	cr_assert_eq(cap_object_acquire_for_use(104u, cap_id, CAP_READ, &acquired, &rights), CAP_OK);
	cr_assert_eq(rights, CAP_READ);
	cr_assert_eq(cap_object_active_call_count(acquired), 0u);
	cr_assert(cap_destroy_by_id(cap_id));
	cr_assert(cap_object_destroy_with_id(object_id));
	cr_assert_eq(direct_use_destroy_count, 0u);
	cr_assert_eq(acquired->object_id, 0x303u);
	cap_object_release(published);
	cr_assert_eq(direct_use_destroy_count, 0u);
	cap_object_release(acquired);
	cr_assert_eq(direct_use_destroy_count, 1u);
}

Test(capability, ending_an_inactive_object_is_an_invariant_failure, .signal = SIGABRT) {
	struct cap_object* object;

	cap_test_setup();
	object = cap_object_acquire(cap_object_create(0x20bu, NULL, NULL));
	cr_assert_not_null(object);
	cap_object_end_call(object);
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
	endpoint = channel_create(30u, false);
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

Test(capability, regrant_and_redrop_reuses_the_pending_zero_grants_event) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* grant;
	cap_object_id_t    object_id;
	uint64_t           delivered_id = 0u;

	cap_test_setup();
	endpoint = channel_create(31u, false);
	object   = create_userspace_object(endpoint, 0x209u, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 32u, CAP_READ, NULL)));
	grant = cap_lookup(cap_create(object_id, 32u, CAP_READ, NULL));
	cr_assert_not_null(grant);
	cr_assert(cap_destroy(grant));
	cr_assert(receive_zero_event(endpoint, &delivered_id));
	cr_assert_eq(delivered_id, 0x209u);
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 31u), CHANNEL_OK);
}

Test(capability, failed_zero_grants_delivery_requeues_the_event_for_retry) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct cap_object* queued;
	cap_object_id_t    object_id;
	uint64_t           delivered_id = 0u;

	cap_test_setup();
	endpoint = channel_create(33u, false);
	object   = create_userspace_object(endpoint, 0x20au, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 34u, CAP_READ, NULL)));
	queued = channel_dequeue_cap_event(endpoint);
	cr_assert_eq(queued, object);
	cr_assert(cap_object_prepare_zero_grants_event(queued, &delivered_id));
	cr_assert_eq(delivered_id, 0x20au);
	cap_object_rollback_zero_grants_event(queued);
	cr_assert(receive_zero_event(endpoint, &delivered_id));
	cr_assert_eq(delivered_id, 0x20au);
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 33u), CHANNEL_OK);
}

Test(capability, regrant_between_prepare_and_commit_makes_the_event_stale) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct cap_object* queued;
	struct capability* grant;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(35u, false);
	object   = create_userspace_object(endpoint, 0x20cu, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 36u, CAP_READ, NULL)));
	queued = channel_dequeue_cap_event(endpoint);
	cr_assert_eq(queued, object);
	cr_assert(cap_object_prepare_zero_grants_event(queued, NULL));
	grant = cap_lookup(cap_create(object_id, 36u, CAP_READ, NULL));
	cr_assert_not_null(grant);
	cr_assert_not(cap_object_commit_zero_grants_event(queued));
	cr_assert(cap_destroy(grant));
	cr_assert(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 35u), CHANNEL_OK);
}

Test(capability, unpublish_between_prepare_and_commit_suppresses_delivery) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct cap_object* queued;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(37u, false);
	object   = create_userspace_object(endpoint, 0x20du, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 38u, CAP_READ, NULL)));
	queued = channel_dequeue_cap_event(endpoint);
	cr_assert_eq(queued, object);
	cr_assert(cap_object_prepare_zero_grants_event(queued, NULL));
	cr_assert(cap_object_unpublish(endpoint, 0x20du));
	cr_assert_not(cap_object_commit_zero_grants_event(queued));
	cr_assert_null(cap_object_acquire(object_id));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 37u), CHANNEL_OK);
}

Test(capability, regrant_and_redrop_between_prepare_and_commit_keeps_the_event_current) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct cap_object* queued;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(39u, false);
	object   = create_userspace_object(endpoint, 0x20eu, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 40u, CAP_READ, NULL)));
	queued = channel_dequeue_cap_event(endpoint);
	cr_assert_eq(queued, object);
	cr_assert(cap_object_prepare_zero_grants_event(queued, NULL));
	cr_assert(cap_destroy_by_id(cap_create(object_id, 40u, CAP_READ, NULL)));
	cr_assert(cap_object_commit_zero_grants_event(queued));
	cr_assert_not(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 39u), CHANNEL_OK);
}

Test(capability, conditional_unpublish_requires_the_object_to_remain_unused) {
	struct channel*    endpoint;
	struct cap_object* unused;
	struct cap_object* granted;
	struct capability* grant;
	cap_object_id_t    unused_id;
	cap_object_id_t    granted_id;

	cap_test_setup();
	endpoint = channel_create(42u, false);
	unused   = create_userspace_object(endpoint, 0x20fu, &unused_id);
	cr_assert(cap_object_unpublish_if_unused(endpoint, 0x20fu));
	cr_assert_null(cap_object_acquire(unused_id));
	cap_object_release(unused);

	granted = create_userspace_object(endpoint, 0x210u, &granted_id);
	grant   = cap_acquire(cap_create(granted_id, 43u, CAP_READ, NULL));
	cr_assert_not_null(grant);
	cr_assert_not(cap_object_unpublish_if_unused(endpoint, 0x210u));
	cr_assert_eq(cap_is_valid(grant), CAP_OK);
	cr_assert_eq(cap_object_lookup(endpoint, 0x210u), granted);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_unpublish_if_unused(endpoint, 0x210u));
	cap_object_release(granted);
	cr_assert_eq(channel_destroy(endpoint, 42u), CHANNEL_OK);
}

Test(capability, conditional_unpublish_preserves_a_grant_that_appears_after_zero_grants) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* grant;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(44u, false);
	object   = create_userspace_object(endpoint, 0x211u, &object_id);
	cr_assert(cap_destroy_by_id(cap_create(object_id, 45u, CAP_READ, NULL)));
	cr_assert(receive_zero_event(endpoint, NULL));
	grant = cap_acquire(cap_create(object_id, 45u, CAP_READ, NULL));
	cr_assert_not_null(grant);
	cr_assert_not(cap_object_unpublish_if_unused(endpoint, 0x211u));
	cr_assert_eq(cap_is_valid(grant), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_unpublish_if_unused(endpoint, 0x211u));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 44u), CHANNEL_OK);
}

Test(capability, conditional_unpublish_rejects_an_active_call_without_mutation) {
	struct channel*    endpoint;
	struct cap_object* object;
	struct cap_object* active = NULL;
	struct capability* grant;
	cap_object_id_t    object_id;

	cap_test_setup();
	endpoint = channel_create(46u, false);
	object   = create_userspace_object(endpoint, 0x212u, &object_id);
	grant    = cap_acquire(cap_create(object_id, 47u, CAP_CALL, NULL));
	cr_assert_eq(cap_object_begin_call(47u, grant, &active, NULL), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert_not(cap_object_unpublish_if_unused(endpoint, 0x212u));
	cr_assert_eq(cap_object_lookup(endpoint, 0x212u), object);
	cap_object_end_call(active);
	cr_assert(receive_zero_event(endpoint, NULL));
	cr_assert(cap_object_unpublish_if_unused(endpoint, 0x212u));
	cap_object_release(object);
	cr_assert_eq(channel_destroy(endpoint, 46u), CHANNEL_OK);
}

Test(capability, kernel_destroy_if_unused_checks_grants_and_active_calls_atomically) {
	struct cap_object* unused;
	struct cap_object* granted;
	struct cap_object* active_object;
	struct cap_object* active = NULL;
	struct capability* grant;
	cap_object_id_t    unused_id;
	cap_object_id_t    granted_id;
	cap_object_id_t    active_id;

	cap_test_setup();
	unused_id = cap_object_create_kernel(0x213u, lifecycle_handler, NULL);
	unused    = cap_object_acquire(unused_id);
	cr_assert(cap_object_destroy_if_unused(unused));
	cr_assert_null(cap_object_acquire(unused_id));
	cap_object_release(unused);

	granted_id = cap_object_create_kernel(0x214u, lifecycle_handler, NULL);
	granted    = cap_object_acquire(granted_id);
	grant      = cap_acquire(cap_create(granted_id, 48u, CAP_READ, NULL));
	cr_assert_not(cap_object_destroy_if_unused(granted));
	cr_assert_eq(cap_is_valid(grant), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert(cap_object_destroy_if_unused(granted));
	cap_object_release(granted);

	active_id     = cap_object_create_kernel(0x215u, lifecycle_handler, NULL);
	active_object = cap_object_acquire(active_id);
	grant         = cap_acquire(cap_create(active_id, 49u, CAP_CALL, NULL));
	cr_assert_eq(cap_object_begin_call(49u, grant, &active, NULL), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	cr_assert_not(cap_object_destroy_if_unused(active_object));
	cap_object_end_call(active);
	cr_assert(cap_object_destroy_if_unused(active_object));
	cap_object_release(active_object);
}

Test(capability, lifecycle_queue_is_lossless_beyond_request_queue_depth) {
	struct channel*    endpoint;
	struct cap_object* objects[LIFECYCLE_MANY_OBJECTS];
	cap_object_id_t    object_ids[LIFECYCLE_MANY_OBJECTS];
	bool               seen[LIFECYCLE_MANY_OBJECTS] = {0};

	cap_test_setup();
	endpoint = channel_create(40u, false);
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
	endpoint = channel_create(50u, false);
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
	endpoint = channel_create(60u, false);
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
	endpoint = channel_create(70u, false);
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
	endpoint = channel_create(80u, false);
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
