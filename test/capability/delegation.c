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
	parent_id                 = cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL);
	parent                    = cap_acquire(parent_id);
	child_id                  = cap_create(object_id, 2u, CAP_READ, parent);
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
	root                      = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL));
	child1                    = cap_lookup(cap_create(object_id, 2u, CAP_READ, root));
	child2                    = cap_lookup(cap_create(object_id, 3u, CAP_READ, root));
	grandchild                = cap_lookup(cap_create(object_id, 4u, CAP_READ, child1));

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
	parent                    = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(parent);
	count_before = capability_count();

	cr_assert_eq(cap_create(object_id, 2u, CAP_READ | CAP_WRITE, parent),
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
	caps[0]                   = cap_lookup(cap_create(object_id, targets[0], CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(caps[0]);

	for (size_t i = 1; i < 8; i++) {
		caps[i] = cap_lookup(cap_create(object_id, targets[i], CAP_READ, caps[i - 1]));
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

	cap_id_t cap_id = cap_create(object_id, 5u, CAP_READ, NULL);
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
	root_id                   = cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL);
	root                      = cap_acquire(root_id);
	child_id                  = cap_create(object_id, 5u, CAP_READ, root);
	child                     = cap_acquire(child_id);
	grandchild_id             = cap_create(object_id, 7u, CAP_READ, child);
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
	root_id         = cap_create(root_object_id, 10u, CAP_READ | CAP_DELEGATE, NULL);
	root            = cap_acquire(root_id);
	child_id        = cap_create(child_object_id, 11u, CAP_READ, root);
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

Test(capability, drop_internal_capability_splices_children_to_parent) {
	struct cap_object* object;
	struct capability* root;
	struct capability* middle;
	struct capability* child1;
	struct capability* child2;
	cap_id_t           middle_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x82u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x82u);
	root                      = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL));
	middle_id                 = cap_create(object_id, 2u, CAP_READ | CAP_DELEGATE, root);
	middle                    = cap_lookup(middle_id);
	child1                    = cap_lookup(cap_create(object_id, 3u, CAP_READ, middle));
	child2                    = cap_lookup(cap_create(object_id, 4u, CAP_READ, middle));
	cr_assert_not_null(root);
	cr_assert_not_null(middle);
	cr_assert_not_null(child1);
	cr_assert_not_null(child2);

	cr_assert(cap_drop(middle));
	cr_assert_null(cap_lookup(middle_id));
	cr_assert_eq(child1->parent, root);
	cr_assert_eq(child2->parent, root);
	cr_assert_eq(cap_is_valid(child1), CAP_OK);
	cr_assert_eq(cap_is_valid(child2), CAP_OK);

	cr_assert(cap_destroy(root));
	cr_assert(cap_object_destroy(object));
}

Test(capability, drop_root_makes_children_independent_roots) {
	struct cap_object* object;
	struct capability* root;
	struct capability* child1;
	struct capability* child2;
	cap_id_t           root_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x83u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x83u);
	root_id                   = cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL);
	root                      = cap_lookup(root_id);
	child1                    = cap_lookup(cap_create(object_id, 2u, CAP_READ, root));
	child2                    = cap_lookup(cap_create(object_id, 3u, CAP_READ, root));
	cr_assert_not_null(root);
	cr_assert_not_null(child1);
	cr_assert_not_null(child2);

	cr_assert(cap_drop(root));
	cr_assert_null(cap_lookup(root_id));
	cr_assert_null(child1->parent);
	cr_assert_null(child2->parent);
	cr_assert_null(child1->next_sibling);
	cr_assert_null(child2->next_sibling);
	cr_assert_eq(cap_is_valid(child1), CAP_OK);
	cr_assert_eq(cap_is_valid(child2), CAP_OK);

	cr_assert(cap_destroy(child1));
	cr_assert(cap_destroy(child2));
	cr_assert(cap_object_destroy(object));
}

Test(capability, drop_for_process_preserves_delegations_owned_by_other_processes) {
	struct cap_object* object;
	struct capability* a;
	struct capability* b;
	struct capability* c;
	struct capability* d;
	cap_id_t           a_id;
	cap_id_t           b_id;
	cap_id_t           c_id;
	cap_id_t           d_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x84u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x84u);
	a_id                      = cap_create(object_id, 42u, CAP_READ | CAP_DELEGATE, NULL);
	a                         = cap_lookup(a_id);
	b_id                      = cap_create(object_id, 7u, CAP_READ | CAP_DELEGATE, a);
	b                         = cap_lookup(b_id);
	c_id                      = cap_create(object_id, 42u, CAP_READ | CAP_DELEGATE, b);
	c                         = cap_lookup(c_id);
	d_id                      = cap_create(object_id, 9u, CAP_READ, c);
	d                         = cap_lookup(d_id);
	cr_assert_not_null(a);
	cr_assert_not_null(b);
	cr_assert_not_null(c);
	cr_assert_not_null(d);

	cap_drop_for_process(42u);
	cr_assert_null(cap_lookup(a_id));
	cr_assert_null(cap_lookup(c_id));
	cr_assert_eq(cap_lookup(b_id), b);
	cr_assert_eq(cap_lookup(d_id), d);
	cr_assert_null(b->parent);
	cr_assert_eq(d->parent, b);
	cr_assert_eq(cap_is_valid(b), CAP_OK);
	cr_assert_eq(cap_is_valid(d), CAP_OK);

	cr_assert(cap_destroy(b));
	cr_assert_null(cap_lookup(d_id));
	cr_assert(cap_object_destroy(object));
}

Test(capability, peer_delegation_creates_sibling_and_survives_source_revoke) {
	struct cap_object* object;
	struct capability* root;
	struct capability* source;
	struct capability* normal;
	struct capability* peer;
	cap_id_t           source_id;
	cap_id_t           normal_id;
	cap_id_t           peer_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x85u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x85u);
	root      = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL));
	source_id = cap_create(object_id, 2u, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, root);
	source    = cap_lookup(source_id);
	normal_id = cap_delegate_create(source, 3u, CAP_READ, false);
	peer_id   = cap_delegate_create(source, 4u, CAP_READ, true);
	normal    = cap_lookup(normal_id);
	peer      = cap_lookup(peer_id);
	cr_assert_not_null(root);
	cr_assert_not_null(source);
	cr_assert_not_null(normal);
	cr_assert_not_null(peer);
	cr_assert_eq(normal->parent, source);
	cr_assert_eq(peer->parent, root);

	cr_assert(cap_destroy(source));
	cr_assert_null(cap_lookup(source_id));
	cr_assert_null(cap_lookup(normal_id));
	cr_assert_eq(cap_lookup(peer_id), peer);
	cr_assert_eq(cap_is_valid(peer), CAP_OK);

	cr_assert(cap_destroy(root));
	cr_assert_null(cap_lookup(peer_id));
	cr_assert(cap_object_destroy(object));
}

Test(capability, peer_delegation_from_root_creates_independent_root) {
	struct cap_object* object;
	struct capability* source;
	struct capability* peer;
	cap_id_t           source_id;
	cap_id_t           peer_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x86u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x86u);
	source_id                 = cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL);
	source                    = cap_lookup(source_id);
	peer_id                   = cap_delegate_create(source, 2u, CAP_READ, true);
	peer                      = cap_lookup(peer_id);
	cr_assert_not_null(source);
	cr_assert_not_null(peer);
	cr_assert_null(peer->parent);

	cr_assert(cap_destroy(source));
	cr_assert_null(cap_lookup(source_id));
	cr_assert_eq(cap_lookup(peer_id), peer);
	cr_assert_eq(cap_is_valid(peer), CAP_OK);

	cr_assert(cap_destroy(peer));
	cr_assert(cap_object_destroy(object));
}

Test(capability, peer_delegation_requires_explicit_peer_right) {
	struct cap_object* object;
	struct capability* source;
	cap_id_t           normal_id;

	cap_test_setup();
	cap_object_id_t object_id = cap_object_create(0x87u, NULL, NULL);
	object                    = cap_object_lookup(NULL, 0x87u);
	source                    = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE, NULL));
	cr_assert_not_null(source);

	cr_assert_eq(cap_delegate_create(source, 2u, CAP_READ, true),
	             CAP_ID_INVALID,
	             "CAP_DELEGATE alone must not permit sibling creation");
	normal_id = cap_delegate_create(source, 2u, CAP_READ, false);
	cr_assert_neq(normal_id, CAP_ID_INVALID);

	cr_assert(cap_destroy(source));
	cr_assert_null(cap_lookup(normal_id));
	cr_assert(cap_object_destroy(object));
}
