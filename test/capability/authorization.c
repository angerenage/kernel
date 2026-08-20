#include "test_support.h"

Test(capability, authorization_direct_target) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cap                       = cap_lookup(cap_create(object_id, 5u, CAP_READ, NULL, NULL));

	result = cap_is_authorized(5u, cap);
	cr_assert_eq(result, CAP_OK, "direct target should be authorized");

	result = cap_is_authorized(6u, cap);
	cr_assert_eq(result, CAP_NOT_AUTHORIZED, "non-target should not be authorized");

	cap_destroy(cap);
	cap_object_destroy(obj);
}

Test(capability, authorization_endpoint_owner) {
	struct channel*    ch;
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	ch = channel_create(10u);
	cr_assert_not_null(ch, "channel_create should succeed");

	cap_object_id_t object_id = cap_object_create(1u, ch, NULL);
	obj                       = cap_object_lookup(ch, 1u);
	cap                       = cap_lookup(cap_create(object_id, 5u, CAP_READ, NULL, NULL));

	result = cap_is_authorized(10u, cap);
	cr_assert_eq(result, CAP_OK, "endpoint owner should be authorized");

	result = cap_is_authorized(11u, cap);
	cr_assert_eq(result, CAP_NOT_AUTHORIZED, "non-owner should not be authorized");

	cap_destroy(cap);
	cap_object_destroy(obj);
	channel_destroy(ch, 10u);
}

Test(capability, authorization_ancestral_chain) {
	struct cap_object* obj;
	struct capability* root;
	struct capability* child;
	struct capability* grandchild;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);

	root       = cap_lookup(cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL));
	child      = cap_lookup(cap_create(object_id, 5u, CAP_READ, root, NULL));
	grandchild = cap_lookup(cap_create(object_id, 7u, CAP_READ, child, NULL));

	result = cap_is_authorized(10u, grandchild);
	cr_assert_eq(result, CAP_OK, "ancestor target should be authorized for grandchild");

	result = cap_is_authorized(10u, child);
	cr_assert_eq(result, CAP_OK, "ancestor target should be authorized for child");

	result = cap_is_authorized(10u, root);
	cr_assert_eq(result, CAP_OK, "direct target should be authorized for root");

	result = cap_is_authorized(5u, grandchild);
	cr_assert_eq(result, CAP_OK, "intermediate target should be authorized for grandchild");

	result = cap_is_authorized(7u, grandchild);
	cr_assert_eq(result, CAP_OK, "direct target should be authorized");

	result = cap_is_authorized(99u, grandchild);
	cr_assert_eq(result, CAP_NOT_AUTHORIZED, "unrelated caller should not be authorized");

	cap_destroy(grandchild);
	cap_destroy(child);
	cap_destroy(root);
	cap_object_destroy(obj);
}

Test(capability, authorization_removed_capability) {
	struct cap_object* obj;
	struct capability* cap;
	cap_id_t           cap_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	cap_id                    = cap_create(object_id, 5u, CAP_READ, NULL, NULL);
	cap                       = cap_acquire(cap_id);
	cr_assert_not_null(cap);

	cr_assert(cap_destroy(cap));
	cr_assert_null(cap_lookup(cap_id));

	result = cap_is_authorized(5u, cap);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed cap should no longer authorize its former target");

	result = cap_is_authorized(6u, cap);
	cr_assert_eq(result, CAP_NOT_FOUND, "removed cap should no longer authorize any caller");

	cap_release(cap);
	cap_object_destroy(obj);
}

Test(capability, authorization_removed_ancestor_removes_descendants) {
	struct cap_object* obj;
	struct capability* root;
	struct capability* child;
	cap_id_t           root_id;
	cap_id_t           child_id;
	enum cap_result    result;

	cap_test_setup();

	cap_object_id_t object_id = cap_object_create(1u, NULL, NULL);
	obj                       = cap_object_lookup(NULL, 1u);
	root_id                   = cap_create(object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	root                      = cap_acquire(root_id);
	child_id                  = cap_create(object_id, 5u, CAP_READ, root, NULL);
	child                     = cap_acquire(child_id);
	cr_assert_not_null(root);
	cr_assert_not_null(child);

	cr_assert(cap_destroy(root));
	cr_assert_null(cap_lookup(root_id));
	cr_assert_null(cap_lookup(child_id));

	result = cap_is_authorized(10u, child);
	cr_assert_eq(result, CAP_NOT_FOUND);

	result = cap_is_authorized(5u, child);
	cr_assert_eq(result, CAP_NOT_FOUND);

	result = cap_is_authorized(99u, child);
	cr_assert_eq(result, CAP_NOT_FOUND);

	cap_release(child);
	cap_release(root);
	cap_object_destroy(obj);
}
