#include "test_support.h"

Test(capability, cap_create_rejects_null_object) {
	cap_test_setup();

	cr_assert_eq(cap_create(CAP_OBJECT_ID_INVALID, 1u, CAP_READ, NULL),
	             CAP_ID_INVALID,
	             "cap_create should reject an invalid object id");
}

Test(capability, cap_create_lookup_destroy) {
	struct cap_object* obj;
	struct capability* cap;
	struct capability* found;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(10u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 10u);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	cr_assert_not_null(obj);

	cap_id_t cap_id = cap_create(object_id, 5u, CAP_READ | CAP_WRITE, NULL);
	cap             = cap_lookup(cap_id);
	cr_assert_neq(cap_id, CAP_ID_INVALID, "cap_create should succeed");
	cr_assert_not_null(cap, "cap_create should succeed");
	cr_assert_neq(cap->cap_id, CAP_ID_INVALID, "cap_id should be valid");
	cr_assert_eq(cap->cap_object_id, obj->cap_object_id, "cap object_id mismatch");
	cr_assert_eq(cap->target, 5u, "cap target mismatch");
	cr_assert_eq(cap->rights, CAP_READ | CAP_WRITE, "cap rights mismatch");
	cr_assert_null(cap->parent, "root cap parent should be NULL");
	cr_assert_not(cap_is_removed(cap), "new cap should be live");

	found = cap_lookup(cap->cap_id);
	cr_assert_eq(found, cap, "cap_lookup should find the created capability");

	cr_assert_eq(capability_object_count(), 1u);
	cr_assert_eq(capability_count(), 1u);

	cr_assert(cap_destroy_by_id(cap_id), "cap_destroy should succeed");
	found = cap_lookup(cap_id);
	cr_assert_null(found, "cap_lookup should return NULL after destroy");

	cap_object_destroy(obj);
	cr_assert_eq(capability_object_count(), 0u);
	cr_assert_eq(capability_count(), 0u);
}

Test(capability, delegation_links_do_not_retain_parent_records) {
	struct cap_object* object;
	struct capability* parent;
	struct capability* child;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(11u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 11u);
	parent                    = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(parent);
	cr_assert_eq(parent->reference_count, 1u, "the table should own the only structural record reference");

	child = cap_lookup(cap_create(object_id, 2u, CAP_READ, parent));
	cr_assert_not_null(child);
	cr_assert_eq(parent->reference_count,
	             1u,
	             "delegation topology must not use reference-count ownership between parent and child");
	cr_assert_eq(child->reference_count, 1u);

	cr_assert(cap_destroy(child));
	cr_assert(cap_destroy(parent));
	cr_assert(cap_object_destroy(object));
}

Test(capability, retained_reference_outlives_table_removal) {
	struct cap_object* object;
	struct capability* cap;
	cap_id_t           cap_id;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(12u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 12u);
	cap_id                    = cap_create(object_id, 1u, CAP_READ, NULL);
	cap                       = cap_acquire(cap_id);
	cr_assert_not_null(cap);
	cr_assert_eq(cap->reference_count, 2u, "table ownership plus one acquired reference should be retained");

	cr_assert(cap_destroy(cap));
	cr_assert_null(cap_lookup(cap_id));
	cr_assert(cap_is_removed(cap));
	cr_assert_eq(cap->reference_count, 1u, "removal should release only the table-owned reference");

	cap_release(cap);
	cr_assert(cap_object_destroy(object));
}

Test(capability, matching_grants_are_distinct_and_independent) {
	struct cap_object* obj;
	struct capability* cap1;
	struct capability* cap2;
	cap_id_t           cap1_id;
	cap_id_t           cap2_id;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(7u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 7u);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	cr_assert_not_null(obj);

	cap1_id = cap_create(object_id, 9u, CAP_READ, NULL);
	cap2_id = cap_create(object_id, 9u, CAP_READ, NULL);
	cap1    = cap_lookup(cap1_id);
	cap2    = cap_lookup(cap2_id);
	cr_assert_not_null(cap1);
	cr_assert_not_null(cap2);
	cr_assert_neq(cap1_id, cap2_id, "matching grants must still receive independent capability IDs");
	cr_assert_neq(cap1, cap2, "matching grants must use independent capability records");
	cr_assert_eq(capability_count(), 2u);

	cr_assert(cap_drop(cap1));
	cr_assert_null(cap_lookup(cap1_id));
	cr_assert_eq(cap_lookup(cap2_id), cap2, "dropping one matching grant must not remove another grant");
	cr_assert_eq(cap_is_valid(cap2), CAP_OK);
	cr_assert_eq(capability_count(), 1u);

	cr_assert(cap_destroy(cap2));
	cr_assert(cap_object_destroy(obj));
}

Test(capability, null_and_invalid_arguments) {
	struct cap_object* obj;
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

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cap_id_t cap_id           = cap_create(object_id, 1u, CAP_READ, NULL);

	cr_assert_null(cap_lookup(999999u), "non-existent ID lookup should return NULL");

	cap_destroy_by_id(cap_id);
	cap_object_destroy(obj);
}
