#include "test_support.h"

Test(capability, destroying_parent_removes_descendant_subtree) {
	struct cap_object* object;
	struct capability* parent;
	struct capability* child;
	cap_id_t           parent_id;
	cap_id_t           child_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(77u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 77u);
	parent_id                 = cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	parent                    = cap_acquire(parent_id);
	child_id                  = cap_create(object_id, 2u, CAP_READ, parent, NULL);
	child                     = cap_acquire(child_id);
	cr_assert_not_null(parent);
	cr_assert_not_null(child);

	cr_assert(cap_destroy(parent));
	cr_assert_null(cap_lookup(parent_id));
	cr_assert_null(cap_lookup(child_id));
	cr_assert_eq(cap_is_valid(child), CAP_NOT_FOUND, "removed descendants may only survive as retained records");
	cap_release(child);
	cap_release(parent);
	cr_assert(cap_object_destroy(object));
}

Test(capability, tree_tracks_children_and_siblings) {
	struct cap_object* object;
	struct capability* root;
	struct capability* child1;
	struct capability* child2;
	struct capability* grandchild;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x78u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x78u);
	root                      = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	child1                    = cap_lookup(cap_create(object_id, 2u, CAP_READ, root, NULL));
	child2                    = cap_lookup(cap_create(object_id, 3u, CAP_READ, root, NULL));
	grandchild                = cap_lookup(cap_create(object_id, 4u, CAP_READ, child1, NULL));

	cr_assert_not_null(root);
	cr_assert_not_null(child1);
	cr_assert_not_null(child2);
	cr_assert_not_null(grandchild);
	cr_assert_eq(root->first_child, child2, "new children should be linked at the head");
	cr_assert_eq(child2->next_sibling, child1);
	cr_assert_null(child1->next_sibling);
	cr_assert_eq(child1->first_child, grandchild);
	cr_assert_eq(grandchild->parent, child1);

	cr_assert(cap_destroy(child2));
	cr_assert_eq(root->first_child, child1, "destroy must unlink the removed child from its parent");
	cr_assert_null(child1->next_sibling);

	cr_assert(cap_destroy(grandchild));
	cr_assert(cap_destroy(child1));
	cr_assert(cap_destroy(root));
	cr_assert(cap_object_destroy(object));
}

Test(capability, child_rights_must_be_subset_of_parent_rights) {
	struct cap_object* object;
	struct capability* parent;
	size_t             count_before;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x79u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x79u);
	parent                    = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	cr_assert_not_null(parent);
	count_before = capability_count();

	cr_assert_eq(cap_create(object_id, 2u, CAP_READ | CAP_WRITE, parent, NULL),
	             CAP_ID_INVALID,
	             "a child must never gain rights absent from its parent");
	cr_assert_eq(capability_count(), count_before);

	cr_assert(cap_destroy(parent));
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

	for (size_t i = 0; i < 8; i++) caps[i] = cap_acquire(caps[i]->cap_id);
	cr_assert(cap_destroy(caps[3]));

	for (size_t i = 0; i <= 3; i++) {
		result = cap_is_valid(caps[i]);
		if (i == 3) {
			cr_assert_eq(result, CAP_NOT_FOUND, "caps[3] should be removed");
		}
		else {
			cr_assert_eq(result, CAP_OK, "caps[%zu] should still be valid", i);
		}
	}
	for (size_t i = 4; i < 8; i++) {
		result = cap_is_valid(caps[i]);
		cr_assert_eq(result, CAP_NOT_FOUND, "caps[%zu] should be removed with its ancestor", i);
	}

	for (size_t i = 0; i < 8; i++) {
		result = cap_is_authorized(targets[i], caps[i]);
		if (i >= 3) {
			cr_assert_eq(result, CAP_NOT_FOUND, "removed subtree targets must not remain authorized");
		}
		else {
			cr_assert_eq(result, CAP_OK, "direct target should be authorized for caps[%zu]", i);
		}
	}
	for (size_t i = 4; i < 8; i++) {
		for (size_t j = 0; j < i; j++) {
			result = cap_is_authorized(targets[j], caps[i]);
			cr_assert_eq(result, CAP_NOT_FOUND, "every caller must observe eager subtree removal");
		}
	}

	for (size_t i = 3; i-- > 0u;) cr_assert(cap_destroy(caps[i]));
	for (size_t i = 3; i < 8; i++) cr_assert_null(cap_lookup(caps[i]->cap_id));
	for (size_t i = 0; i < 8; i++) cap_release(caps[i]);
	cap_object_destroy(obj);
}

Test(capability, object_destroy_invalidates_capabilities) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(2u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 2u);
	cr_assert_not_null(obj);

	cap_id_t cap_id = cap_create(object_id, 5u, CAP_READ, NULL, NULL);
	cap             = cap_acquire(cap_id);
	cr_assert_not_null(cap);

	cr_assert(cap_object_alive(cap), "cap should be backed by a live object");
	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_OK, "live cap should be valid");

	cr_assert(cap_object_destroy_with_id(object_id), "destroy by id should succeed");
	cr_assert_eq(capability_object_count(), 0u, "object table should be empty");

	cr_assert_eq(capability_count(), 0u, "capabilities of a destroyed object should be removed immediately");
	cr_assert_null(cap_lookup(cap_id));

	cr_assert_not(cap_object_alive(cap), "retained removed record should not report a live capability object");
	result = cap_is_valid(cap);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed capability should no longer be valid");
	result = cap_is_authorized(5u, cap);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed capability should no longer authorize its target");

	cap_release(cap);
}

Test(capability, object_destroy_invalidates_delegation_chain) {
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	cap_id_t           root_id;
	cap_id_t           child_id;
	cap_id_t           grandchild_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(3u, NULL, NULL);
	root_id                   = cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	root                      = cap_acquire(root_id);
	child_id                  = cap_create(object_id, 5u, CAP_READ, root, NULL);
	child                     = cap_acquire(child_id);
	grandchild_id             = cap_create(object_id, 7u, CAP_READ, child, NULL);
	grandchild                = cap_acquire(grandchild_id);

	cr_assert(cap_object_destroy_with_id(object_id), "destroy by id should succeed");
	cr_assert_null(cap_lookup(root_id));
	cr_assert_null(cap_lookup(child_id));
	cr_assert_null(cap_lookup(grandchild_id));

	result = cap_is_valid(grandchild);
	cr_assert_eq(result, CAP_NOT_FOUND);
	result = cap_is_valid(child);
	cr_assert_eq(result, CAP_NOT_FOUND);
	result = cap_is_valid(root);
	cr_assert_eq(result, CAP_NOT_FOUND);

	result = cap_is_authorized(7u, grandchild);
	cr_assert_eq(result, CAP_NOT_FOUND);

	cap_release(grandchild);
	cap_release(child);
	cap_release(root);
}

Test(capability, ancestor_object_destroy_is_propagated_to_derived_descendants) {
	struct cap_object* root_object;
	struct cap_object* child_object;
	struct capability* root;
	struct capability* child;
	cap_id_t           root_id;
	cap_id_t           child_id;
	cap_object_id_t    root_object_id;
	cap_object_id_t    child_object_id;

	cap_test_setup();
	root_object_id  = cap_object_create(0x80u, NULL, NULL);
	child_object_id = cap_object_create(0x81u, NULL, NULL);
	root_object     = cap_object_lookup(NULL, 0x80u);
	child_object    = cap_object_lookup(NULL, 0x81u);
	root_id         = cap_create(root_object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	root            = cap_acquire(root_id);
	child_id        = cap_create(child_object_id, 11u, CAP_READ, root, NULL);
	child           = cap_acquire(child_id);
	cr_assert_not_null(root);
	cr_assert_not_null(child);

	cr_assert(cap_object_destroy(root_object));
	cr_assert_null(cap_lookup(root_id));
	cr_assert_null(cap_lookup(child_id));
	cr_assert_eq(cap_is_valid(root), CAP_NOT_FOUND);
	cr_assert_eq(cap_is_valid(child), CAP_NOT_FOUND, "ancestor object removal should remove the descendant branch");

	cap_release(child);
	cap_release(root);
	cr_assert(cap_object_destroy(child_object));
}
