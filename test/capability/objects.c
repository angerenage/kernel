#include "test_support.h"

Test(capability, init_resets_counts) {
	cap_test_setup();
	cr_assert_eq(capability_object_count(), 0u, "object count should be zero after init");
	cr_assert_eq(capability_count(), 0u, "capability count should be zero after init");
}

Test(capability, object_create_and_lookup) {
	struct cap_object* obj1;
	struct cap_object* obj2;
	struct cap_object* found;

	cap_test_setup();

	cap_object_id_t obj1_id = cap_object_create(42u, NULL, NULL);
	obj1                    = cap_object_lookup(NULL, 42u);
	cr_assert_neq(obj1_id, CAP_OBJECT_ID_INVALID, "cap_object_create should succeed");
	cr_assert_not_null(obj1, "cap_object_create should succeed");
	cr_assert_eq(obj1->object_id, 42u, "object_id mismatch");
	cr_assert_null(obj1->endpoint, "endpoint should be NULL for kernel object");

	found = cap_object_lookup(NULL, 42u);
	cr_assert_eq(found, obj1, "cap_object_lookup should find the created object");

	cap_object_id_t obj2_id = cap_object_create(42u, NULL, NULL);
	obj2                    = cap_object_lookup(NULL, 42u);
	cr_assert_eq(obj2_id, obj1_id, "deduplication should return the existing object ID");
	cr_assert_not_null(obj2, "second cap_object_create should succeed");
	cr_assert_eq(obj2, obj1, "cap_object_create should deduplicate on (object_id, endpoint)");

	found = cap_object_lookup(NULL, 42u);
	cr_assert_eq(found, obj1, "cap_object_lookup should still return the original object");

	found = cap_object_lookup(NULL, 99u);
	cr_assert_null(found, "cap_object_lookup should return NULL for non-existent object");

	cap_object_destroy(obj1);
}

Test(capability, object_destroy) {
	struct cap_object* obj;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	cr_assert_not_null(obj);
	cr_assert_eq(capability_object_count(), 1u);

	cr_assert(cap_object_destroy(obj), "cap_object_destroy should succeed");
	cr_assert_eq(capability_object_count(), 0u, "object count should be zero after destroy");

	cr_assert_not(cap_object_destroy(NULL), "cap_object_destroy should reject NULL");
	cr_assert_not(cap_object_destroy(obj), "double destroy should fail");
}

Test(capability, counts_track_allocations) {
	cap_object_id_t objs[4];
	cap_id_t        caps[4];

	cap_test_setup();

	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_eq(capability_count(), 0u);

	for (size_t i = 0; i < 4; i++) {
		objs[i] = cap_object_create(i, NULL, NULL);
		cr_assert_neq(objs[i], CAP_OBJECT_ID_INVALID);
		cr_assert_eq(capability_object_count(), i + 1, "object count mismatch at %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		caps[i] = cap_create(objs[i], 1u, CAP_READ, NULL, NULL);
		cr_assert_neq(caps[i], CAP_ID_INVALID);
		cr_assert_eq(capability_count(), i + 1, "capability count mismatch at %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		cap_destroy_by_id(caps[i]);
		cr_assert_eq(capability_count(), 3 - i, "capability count mismatch after destroy %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		cap_object_destroy_with_id(objs[i]);
		cr_assert_eq(capability_object_count(), 3 - i, "object count mismatch after destroy %zu", i);
	}
}

Test(capability, create_rejects_unknown_object_ids) {
	cap_test_setup();

	cr_assert_eq(cap_create((cap_object_id_t)0x7fffffffu, 1u, CAP_READ, NULL, NULL),
	             CAP_ID_INVALID,
	             "a capability must not be created for an unregistered cap_object");
	cr_assert_eq(capability_count(), 0u, "rejected creation must not leave a dead capability record");
}

Test(capability, destroyed_object_id_cannot_create_a_dead_capability) {
	cap_object_id_t object_id;

	cap_test_setup();
	object_id = cap_object_create(0x102u, NULL, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);

	cr_assert(cap_object_destroy_with_id(object_id), "failed to unregister test cap_object");
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_eq(cap_create(object_id, 2u, CAP_READ, NULL, NULL),
	             CAP_ID_INVALID,
	             "cap_create must reject an object id after that object is unregistered");
	cr_assert_eq(capability_count(), 0u, "dead-object creation leaked an unusable capability");
}

Test(capability, acquired_object_survives_unregistration_until_release) {
	struct cap_object* held;
	cap_object_id_t    id;

	cap_test_setup();
	id = cap_object_create(0x105u, NULL, NULL);
	cr_assert_neq(id, CAP_OBJECT_ID_INVALID);
	held = cap_object_acquire(id);
	cr_assert_not_null(held);

	cr_assert(cap_object_destroy_with_id(id), "object unregistration failed");
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_null(cap_object_acquire(id), "unregistered object must reject new acquisitions");
	cr_assert_eq(held->object_id,
	             0x105u,
	             "an already-acquired object must remain alive until its retained reference is released");
	cap_object_release(held);
}
