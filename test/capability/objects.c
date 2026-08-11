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

	obj1 = cap_object_create(42u, NULL);
	cr_assert_not_null(obj1, "cap_object_create should succeed");
	cr_assert_eq(obj1->object_id, 42u, "object_id mismatch");
	cr_assert_null(obj1->endpoint, "endpoint should be NULL for kernel object");

	found = cap_object_lookup(NULL, 42u);
	cr_assert_eq(found, obj1, "cap_object_lookup should find the created object");

	obj2 = cap_object_create(42u, NULL);
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

	obj = cap_object_create(1u, NULL);
	cr_assert_not_null(obj);
	cr_assert_eq(capability_object_count(), 1u);

	cr_assert(cap_object_destroy(obj), "cap_object_destroy should succeed");
	cr_assert_eq(capability_object_count(), 0u, "object count should be zero after destroy");

	cr_assert_not(cap_object_destroy(NULL), "cap_object_destroy should reject NULL");
	cr_assert_not(cap_object_destroy(obj), "double destroy should fail");
}

Test(capability, counts_track_allocations) {
	struct cap_object* objs[4];
	struct capability* caps[4];

	cap_test_setup();

	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_eq(capability_count(), 0u);

	for (size_t i = 0; i < 4; i++) {
		objs[i] = cap_object_create(i, NULL);
		cr_assert_not_null(objs[i]);
		cr_assert_eq(capability_object_count(), i + 1, "object count mismatch at %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		caps[i] = cap_create(objs[i]->cap_object_id, 1u, CAP_READ, NULL);
		cr_assert_not_null(caps[i]);
		cr_assert_eq(capability_count(), i + 1, "capability count mismatch at %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		cap_destroy(caps[i]);
		cr_assert_eq(capability_count(), 3 - i, "capability count mismatch after destroy %zu", i);
	}

	for (size_t i = 0; i < 4; i++) {
		cap_object_destroy(objs[i]);
		cr_assert_eq(capability_object_count(), 3 - i, "object count mismatch after destroy %zu", i);
	}
}

Test(capability, create_rejects_unknown_object_ids) {
	cap_test_setup();

	cr_assert_null(cap_create((cap_object_id_t)0x7fffffffu, 1u, CAP_READ, NULL),
	               "a capability must not be created for an unregistered cap_object");
	cr_assert_eq(capability_count(), 0u, "rejected creation must not leave a dead capability record");
}

Test(capability, destroyed_object_id_cannot_create_a_dead_capability) {
	struct cap_object* object;
	cap_object_id_t    object_id;

	cap_test_setup();
	object = cap_object_create(0x102u, NULL);
	cr_assert_not_null(object);
	object_id = object->cap_object_id;

	cr_assert(cap_object_destroy_with_id(object_id), "failed to unregister test cap_object");
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_null(cap_create(object_id, 2u, CAP_READ, NULL),
	               "cap_create must reject an object id after that object is unregistered");
	cr_assert_eq(capability_count(), 0u, "dead-object creation leaked an unusable capability");
}

Test(capability, acquired_object_survives_unregistration_until_release) {
	struct cap_object* object;
	struct cap_object* held;
	cap_object_id_t    id;

	cap_test_setup();
	object = cap_object_create(0x105u, NULL);
	cr_assert_not_null(object);
	id   = object->cap_object_id;
	held = cap_object_acquire(id);
	cr_assert_eq(held, object);

	cr_assert(cap_object_destroy_with_id(id), "object unregistration failed");
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_null(cap_object_acquire(id), "unregistered object must reject new acquisitions");
	cr_assert_eq(held->object_id,
	             0x105u,
	             "an already-acquired object must remain alive until its retained reference is released");
	cap_object_release(held);
}
