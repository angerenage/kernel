#include "test_support.h"

Test(capability, destroying_parent_keeps_descendant_chain_safe_and_revoked) {
	struct cap_object* object;
	struct capability* parent;
	struct capability* child;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(77u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 77u);
	parent                    = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	child                     = cap_lookup(cap_create(object_id, 2u, CAP_READ, parent, NULL));
	cr_assert_not_null(parent);
	cr_assert_not_null(child);

	cr_assert(cap_destroy(parent));
	cr_assert_eq(cap_is_valid(child), CAP_REVOKED, "destroying a parent must invalidate its descendants");
	cr_assert(cap_destroy(child));
	cr_assert(cap_object_destroy(object));
}

Test(capability, deep_parent_chain) {
	struct cap_object* obj;
	struct capability* caps[8];
	enum cap_result    result;
	process_id_t       targets[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	caps[0]                   = cap_lookup(cap_create(object_id, targets[0], CAP_READ | CAP_DELEGATE, NULL, NULL));
	cr_assert_not_null(caps[0]);

	for (size_t i = 1; i < 8; i++) {
		caps[i] = cap_lookup(cap_create(object_id, targets[i], CAP_READ, caps[i - 1], NULL));
		cr_assert_not_null(caps[i], "deep chain allocation %zu failed", i);
	}

	for (size_t i = 0; i < 8; i++) {
		for (size_t j = 0; j < 8; j++) {
			result = cap_is_authorized(targets[j], caps[i]);
			if (j <= i) {
				cr_assert_eq(result, CAP_OK, "target[%zu] should be authorized for caps[%zu]", j, i);
			}
			else {
				cr_assert_eq(result, CAP_NOT_AUTHORIZED, "target[%zu] should NOT be authorized for caps[%zu]", j, i);
			}
		}
	}

	for (size_t i = 0; i < 8; i++) {
		result = cap_is_valid(caps[i]);
		cr_assert_eq(result, CAP_OK, "caps[%zu] should be valid", i);
	}

	caps[3]->revoked = true;

	for (size_t i = 0; i <= 3; i++) {
		result = cap_is_valid(caps[i]);
		if (i == 3) {
			cr_assert_eq(result, CAP_REVOKED, "caps[3] should be invalid after self-revoke");
		}
		else {
			cr_assert_eq(result, CAP_OK, "caps[%zu] should still be valid", i);
		}
	}
	for (size_t i = 4; i < 8; i++) {
		result = cap_is_valid(caps[i]);
		cr_assert_eq(result, CAP_REVOKED, "caps[%zu] should be invalid due to revoked ancestor", i);
	}

	for (size_t i = 0; i < 8; i++) {
		result = cap_is_authorized(targets[i], caps[i]);
		if (i == 3) {
			cr_assert_eq(result, CAP_REVOKED, "direct target of a self-revoked cap should get CAP_REVOKED");
		}
		else {
			cr_assert_eq(result, CAP_OK, "direct target should be authorized for caps[%zu]", i);
		}
	}
	for (size_t i = 4; i < 8; i++) {
		for (size_t j = 0; j < i; j++) {
			result = cap_is_authorized(targets[j], caps[i]);
			if (j >= 4) {
				cr_assert_eq(result, CAP_OK, "ancestor caps[%zu] target should be authorized for caps[%zu]", j, i);
			}
			else {
				cr_assert_eq(
					result, CAP_REVOKED, "ancestor walk should hit revoked caps[3] for caps[%zu] (j=%zu)", i, j);
			}
		}
	}

	for (size_t i = 0; i < 8; i++) {
		cap_destroy(caps[i]);
	}
	cap_object_destroy(obj);
}

Test(capability, object_destroy_invalidates_capabilities) {
	struct cap_object* obj;
	struct capability* cap;
	struct capability* found;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(2u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 2u);
	cr_assert_not_null(obj);

	cap_id_t cap_id = cap_create(object_id, 5u, CAP_READ, NULL, NULL);
	cap             = cap_lookup(cap_id);
	cr_assert_not_null(cap);

	cr_assert(cap_object_alive(cap), "cap should be backed by a live object");
	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OK, "live cap should be valid");

	cr_assert(cap_object_destroy_with_id(object_id), "destroy by id should succeed");
	cr_assert_eq(capability_object_count(), 0u, "object table should be empty");

	cr_assert_eq(capability_count(), 1u, "capability record should linger until reap");
	found = cap_lookup(cap->cap_id);
	cr_assert_eq(found, cap, "cap_lookup should still return the record");

	cr_assert_not(cap_object_alive(cap), "cap should report dead object");
	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "validity should report destroyed object");
	result = cap_is_authorized(5u, cap);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "authorization should report destroyed object");

	cr_assert(cap_destroy_by_id(cap_id), "destroy by id should reap the dangling cap");
	cr_assert_eq(capability_count(), 0u, "capability table should be empty after reap");
}

Test(capability, object_destroy_invalidates_delegation_chain) {
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(3u, NULL, NULL);
	root                      = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	child                     = cap_lookup(cap_create(object_id, 5u, CAP_READ, root, NULL));
	grandchild                = cap_lookup(cap_create(object_id, 7u, CAP_READ, child, NULL));

	cr_assert(cap_object_destroy_with_id(object_id), "destroy by id should succeed");

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "grandchild should report destroyed object");
	result = cap_is_valid(child);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "child should report destroyed object");
	result = cap_is_valid(root);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "root should report destroyed object");

	result = cap_is_authorized(7u, grandchild);
	cr_assert_eq(result, CAP_OBJECT_DESTROYED, "authorization should propagate destroyed state");

	cap_destroy(grandchild);
	cap_destroy(child);
	cap_destroy(root);
}
