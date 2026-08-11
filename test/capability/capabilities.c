#include "test_support.h"

Test(capability, cap_create_rejects_null_object) {
	struct capability* cap;

	cap_test_setup();

	cap = cap_create(CAP_OBJECT_ID_INVALID, 1u, CAP_READ, NULL);
	cr_assert_null(cap, "cap_create should reject an invalid object id");
}

Test(capability, cap_create_lookup_destroy) {
	struct cap_object* obj;
	struct capability* cap;
	struct capability* found;

	cap_test_setup();

	obj = cap_object_create(10u, NULL);
	cr_assert_not_null(obj);

	cap = cap_create(obj->cap_object_id, 5u, CAP_READ | CAP_WRITE, NULL);
	cr_assert_not_null(cap, "cap_create should succeed");
	cr_assert_neq(cap->cap_id, CAP_ID_INVALID, "cap_id should be valid");
	cr_assert_eq(cap->cap_object_id, obj->cap_object_id, "cap object_id mismatch");
	cr_assert_eq(cap->target, 5u, "cap target mismatch");
	cr_assert_eq(cap->rights, CAP_READ | CAP_WRITE, "cap rights mismatch");
	cr_assert_null(cap->parent, "root cap parent should be NULL");
	cr_assert_not(cap->revoked, "new cap should not be revoked");

	found = cap_lookup(cap->cap_id);
	cr_assert_eq(found, cap, "cap_lookup should find the created capability");

	cr_assert_eq(capability_object_count(), 1u);
	cr_assert_eq(capability_count(), 1u);

	cr_assert(cap_destroy(cap), "cap_destroy should succeed");
	found = cap_lookup(cap->cap_id);
	cr_assert_null(found, "cap_lookup should return NULL after destroy");

	cap_object_destroy(obj);
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_eq(capability_count(), 0u);
}

Test(capability, dedup_returns_existing_cap) {
	struct cap_object* obj;
	struct capability* cap1;
	struct capability* cap2;

	cap_test_setup();

	obj = cap_object_create(7u, NULL);
	cr_assert_not_null(obj);

	cap1 = cap_create(obj->cap_object_id, 9u, CAP_READ, NULL);
	cr_assert_not_null(cap1);
	cr_assert_eq(capability_count(), 1u);

	cap2 = cap_create(obj->cap_object_id, 9u, CAP_READ, NULL);
	cr_assert_eq(cap2, cap1, "dedup should return the existing capability for matching key");
	cr_assert_eq(capability_count(), 1u, "dedup should not allocate a second capability");

	cap1 = cap_create(obj->cap_object_id, 10u, CAP_READ, NULL);
	cr_assert_neq(cap1, cap2, "different target should allocate a new capability");
	cr_assert_eq(capability_count(), 2u);

	cap2 = cap_create(obj->cap_object_id, 9u, CAP_WRITE, NULL);
	cr_assert_neq(cap2, cap1, "different rights should allocate a new capability");
	cr_assert_eq(capability_count(), 3u);

	cap_destroy(cap2);
	cap_destroy(cap1);
	cap_object_destroy(obj);
}

Test(capability, null_and_invalid_arguments) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	result = cap_is_authorized(PROCESS_PID_INVALID, NULL);
	cr_assert_eq(result, CAP_INVALID_ARGUMENTS, "invalid pid and NULL cap");

	result = cap_is_authorized(1u, NULL);
	cr_assert_eq(result, CAP_INVALID_ARGUMENTS, "NULL cap");

	result = cap_is_valid(NULL);
	cr_assert_eq(result, CAP_INVALID_ARGUMENTS, "NULL cap validity");

	cr_assert_null(cap_lookup(CAP_ID_INVALID), "invalid ID lookup should return NULL");
	cr_assert_not(cap_destroy(NULL), "NULL cap destroy should return false");
	cr_assert_not(cap_object_destroy(NULL), "NULL object destroy should return false");

	obj = cap_object_create(1u, NULL);
	cap = cap_create(obj->cap_object_id, 1u, CAP_READ, NULL);

	cr_assert_null(cap_lookup(999999u), "non-existent ID lookup should return NULL");

	cap_destroy(cap);
	cap_object_destroy(obj);
}
