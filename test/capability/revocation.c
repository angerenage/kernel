#include "test_support.h"

Test(capability, validity_revoked_self) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cap                       = cap_lookup(cap_create(object_id, 5u, CAP_READ, NULL, NULL));

	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OK, "valid cap should return CAP_OK");

	cap->revoked = true;
	result       = cap_is_valid(cap);
	cr_assert_eq(result, CAP_REVOKED, "revoked cap should return CAP_REVOKED");

	cap_destroy(cap);
	cap_object_destroy(obj);
}

Test(capability, validity_revoked_ancestor) {
	struct cap_object* obj;
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	root                      = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	child                     = cap_lookup(cap_create(object_id, 5u, CAP_READ, root, NULL));
	grandchild                = cap_lookup(cap_create(object_id, 7u, CAP_READ, child, NULL));

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_OK, "chain should be valid");

	root->revoked = true;

	result = cap_is_valid(root);
	cr_assert_eq(result, CAP_REVOKED, "revoked root should be invalid");

	result = cap_is_valid(child);
	cr_assert_eq(result, CAP_REVOKED, "child of revoked root should be invalid");

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_REVOKED, "grandchild of revoked root should be invalid");

	cap_destroy(grandchild);
	cap_destroy(child);
	cap_destroy(root);
	cap_object_destroy(obj);
}

Test(capability, revoke_for_process_marks_target_caps) {
	struct cap_object* obj;
	struct capability* targeted;
	struct capability* other;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(4u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 4u);

	targeted = cap_lookup(cap_create(object_id, 42u, CAP_READ, NULL, NULL));
	other    = cap_lookup(cap_create(object_id, 99u, CAP_READ, NULL, NULL));

	cr_assert_eq(cap_is_valid(targeted), CAP_OK, "targeted cap starts valid");
	cr_assert_eq(cap_is_valid(other), CAP_OK, "other cap starts valid");

	cap_revoke_for_process(42u);

	cr_assert_eq(cap_is_valid(targeted), CAP_REVOKED, "targeted cap should be revoked");
	cr_assert_eq(cap_is_valid(other), CAP_OK, "other cap should be untouched");

	result = cap_is_authorized(42u, targeted);
	cr_assert_eq(result, CAP_REVOKED, "revoked target should not authorize its own PID");

	cap_destroy(targeted);
	cap_destroy(other);
	cap_object_destroy(obj);
}

Test(capability, revoke_for_process_is_noop_for_unknown_pid) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(5u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 5u);
	cap                       = cap_lookup(cap_create(object_id, 42u, CAP_READ, NULL, NULL));

	cap_revoke_for_process(1234u);

	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OK, "unknown PID must not affect unrelated caps");

	cap_destroy(cap);
	cap_object_destroy(obj);
}

Test(capability, regrant_after_full_revoke_creates_a_fresh_live_capability) {
	struct cap_object* object;
	struct capability* original;
	struct capability* descendant;
	struct capability* replacement;
	cap_id_t           original_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x103u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x103u);
	cr_assert_not_null(object);
	original = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	cr_assert_not_null(original);
	original_id = original->cap_id;
	descendant  = cap_lookup(cap_create(object_id, 11u, CAP_READ, original, NULL));
	cr_assert_not_null(descendant);

	cap_revoke_for_process(10u);
	cr_assert_eq(cap_is_valid(original), CAP_REVOKED);
	cr_assert_eq(cap_is_valid(descendant), CAP_REVOKED);

	replacement = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	cr_assert_not_null(replacement, "a later legitimate grant must still be possible");
	cr_assert_neq(replacement, original, "regrant must not return the revoked capability record");
	cr_assert_neq(replacement->cap_id, original_id, "regrant must receive an independent capability id");
	cr_assert_eq(cap_is_valid(replacement), CAP_OK, "freshly regranted capability must be usable");
	cr_assert_eq(
		cap_is_valid(descendant), CAP_REVOKED, "regranting the root must not revive descendants of the revoked record");

	cr_assert(cap_destroy(replacement));
	cr_assert(cap_destroy(descendant));
	cr_assert(cap_destroy(original));
	cr_assert(cap_object_destroy(object));
}

Test(capability, revoked_parent_cannot_gain_new_descendants) {
	struct cap_object* object;
	struct capability* parent;
	size_t             count_before;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x104u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x104u);
	cr_assert_not_null(object);
	parent = cap_lookup(cap_create(object_id, 20u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	cr_assert_not_null(parent);

	cap_revoke_for_process(20u);
	cr_assert_eq(cap_is_valid(parent), CAP_REVOKED);
	count_before = capability_count();
	cr_assert_eq(cap_create(object_id, 21u, CAP_READ, parent, NULL),
	             CAP_ID_INVALID,
	             "cap_create must not attach a new child below a revoked parent");
	cr_assert_eq(
		capability_count(), count_before, "rejected child creation must not leave an already-invalid capability");

	cr_assert(cap_destroy(parent));
	cr_assert(cap_object_destroy(object));
}
