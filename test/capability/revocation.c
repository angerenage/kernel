#include "test_support.h"

Test(capability, validity_removed_self) {
	struct cap_object* obj;
	struct capability* cap;
	cap_id_t           cap_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cap_id                    = cap_create(object_id, 5u, CAP_READ, NULL);
	cap                       = cap_acquire(cap_id);

	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OK, "valid cap should return CAP_OK");

	cr_assert(cap_destroy(cap));
	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed cap should return CAP_NOT_FOUND");
	cr_assert_null(cap_lookup(cap_id));

	cap_release(cap);
	cap_object_destroy(obj);
}

Test(capability, validity_removed_ancestor) {
	struct cap_object* obj;
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	cap_id_t           root_id;
	cap_id_t           child_id;
	cap_id_t           grandchild_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	root_id                   = cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL);
	root                      = cap_acquire(root_id);
	child_id                  = cap_create(object_id, 5u, CAP_READ, root);
	child                     = cap_acquire(child_id);
	grandchild_id             = cap_create(object_id, 7u, CAP_READ, child);
	grandchild                = cap_acquire(grandchild_id);

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_OK, "chain should be valid");

	cr_assert(cap_destroy(root));

	result = cap_is_valid(root);
	cr_assert_eq(result, CAP_NOT_FOUND);

	result = cap_is_valid(child);
	cr_assert_eq(result, CAP_NOT_FOUND);

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_NOT_FOUND);
	cr_assert(cap_is_removed(child));
	cr_assert(cap_is_removed(grandchild));
	cr_assert_null(cap_lookup(root_id));
	cr_assert_null(cap_lookup(child_id));
	cr_assert_null(cap_lookup(grandchild_id));

	cap_release(grandchild);
	cap_release(child);
	cap_release(root);
	cap_object_destroy(obj);
}

Test(capability, removing_rights_propagates_to_descendants) {
	struct cap_object* object;
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x106u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x106u);
	root                      = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_WRITE | CAP_DELEGATE, NULL));
	child                     = cap_lookup(cap_create(object_id, 2u, CAP_READ | CAP_WRITE, root));
	grandchild                = cap_lookup(cap_create(object_id, 3u, CAP_WRITE, child));
	cr_assert_not_null(root);
	cr_assert_not_null(child);
	cr_assert_not_null(grandchild);

	cr_assert(cap_remove_rights(root, CAP_WRITE));
	cr_assert_eq(cap_rights(root), CAP_READ | CAP_DELEGATE);
	cr_assert_eq(cap_rights(child), CAP_READ);
	cr_assert_eq(cap_rights(grandchild), 0u);
	cr_assert_eq(cap_is_valid(grandchild), CAP_OK, "losing all rights does not itself revoke a capability");

	cr_assert(cap_destroy(grandchild));
	cr_assert(cap_destroy(child));
	cr_assert(cap_destroy(root));
	cr_assert(cap_object_destroy(object));
}

Test(capability, drop_for_process_removes_target_caps) {
	struct cap_object* obj;
	struct capability* targeted;
	struct capability* other;
	cap_id_t           targeted_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(4u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 4u);

	targeted_id = cap_create(object_id, 42u, CAP_READ, NULL);
	targeted    = cap_acquire(targeted_id);
	other       = cap_lookup(cap_create(object_id, 99u, CAP_READ, NULL));

	cr_assert_eq(cap_is_valid(targeted), CAP_OK, "targeted cap starts valid");
	cr_assert_eq(cap_is_valid(other), CAP_OK, "other cap starts valid");

	cap_drop_for_process(42u);

	cr_assert_eq(cap_is_valid(targeted), CAP_NOT_FOUND, "targeted cap should be removed");
	cr_assert_null(cap_lookup(targeted_id));
	cr_assert_eq(cap_is_valid(other), CAP_OK, "other cap should be untouched");

	result = cap_is_authorized(42u, targeted);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed target should not authorize its own PID");

	cap_release(targeted);
	cap_destroy(other);
	cap_object_destroy(obj);
}

Test(capability, drop_for_process_is_noop_for_unknown_pid) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(5u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 5u);
	cap                       = cap_lookup(cap_create(object_id, 42u, CAP_READ, NULL));

	cap_drop_for_process(1234u);

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
	cap_id_t           descendant_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x103u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x103u);
	cr_assert_not_null(object);
	original_id = cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL);
	original    = cap_acquire(original_id);
	cr_assert_not_null(original);
	descendant_id = cap_create(object_id, 11u, CAP_READ, original);
	descendant    = cap_acquire(descendant_id);
	cr_assert_not_null(descendant);

	cr_assert(cap_destroy(original));
	cr_assert_eq(cap_is_valid(original), CAP_NOT_FOUND);
	cr_assert_eq(cap_is_valid(descendant), CAP_NOT_FOUND);
	cr_assert_null(cap_lookup(original_id));
	cr_assert_null(cap_lookup(descendant_id));

	replacement = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(replacement, "a later legitimate grant must still be possible");
	cr_assert_neq(replacement, original, "regrant must not return the revoked capability record");
	cr_assert_neq(replacement->cap_id, original_id, "regrant must receive an independent capability id");
	cr_assert_eq(cap_is_valid(replacement), CAP_OK, "freshly regranted capability must be usable");
	cr_assert_eq(cap_is_valid(descendant), CAP_NOT_FOUND, "regranting must not revive removed descendants");

	cr_assert(cap_destroy(replacement));
	cap_release(descendant);
	cap_release(original);
	cr_assert(cap_object_destroy(object));
}

Test(capability, removed_parent_cannot_gain_new_descendants) {
	struct cap_object* object;
	struct capability* parent;
	cap_id_t           parent_id;
	size_t             count_before;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x104u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x104u);
	cr_assert_not_null(object);
	parent_id = cap_create(object_id, 20u, CAP_READ | CAP_DELEGATE, NULL);
	parent    = cap_acquire(parent_id);
	cr_assert_not_null(parent);

	cr_assert(cap_destroy(parent));
	cr_assert_eq(cap_is_valid(parent), CAP_NOT_FOUND);
	cr_assert_null(cap_lookup(parent_id));
	count_before = capability_count();
	cr_assert_eq(cap_create(object_id, 21u, CAP_READ, parent),
	             CAP_ID_INVALID,
	             "cap_create must not attach a new child below a removed parent");
	cr_assert_eq(
		capability_count(), count_before, "rejected child creation must not leave an already-invalid capability");

	cap_release(parent);
	cr_assert(cap_object_destroy(object));
}
