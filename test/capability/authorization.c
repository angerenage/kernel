#include "test_support.h"

Test(capability, authorization_direct_target) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	obj = cap_object_create(1u, NULL, NULL);
	cap = cap_create(obj->cap_object_id, 5u, CAP_READ, NULL, NULL);

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

	obj = cap_object_create(1u, ch, NULL);
	cap = cap_create(obj->cap_object_id, 5u, CAP_READ, NULL, NULL);

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

	obj = cap_object_create(1u, NULL, NULL);

	root       = cap_create(obj->cap_object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	child      = cap_create(obj->cap_object_id, 5u, CAP_READ, root, NULL);
	grandchild = cap_create(obj->cap_object_id, 7u, CAP_READ, child, NULL);

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

Test(capability, authorization_revoked_capability) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	obj = cap_object_create(1u, NULL, NULL);
	cap = cap_create(obj->cap_object_id, 5u, CAP_READ, NULL, NULL);

	cap->revoked = true;

	result = cap_is_authorized(5u, cap);
	cr_assert_eq(result, CAP_REVOKED, "revoked cap should return CAP_REVOKED even for direct target");

	result = cap_is_authorized(6u, cap);
	cr_assert_eq(result, CAP_REVOKED, "revoked cap should return CAP_REVOKED for everyone");

	cap_destroy(cap);
	cap_object_destroy(obj);
}

Test(capability, authorization_revoked_ancestor) {
	struct cap_object* obj;
	struct capability* root;
	struct capability* child;
	enum cap_result    result;

	cap_test_setup();

	obj   = cap_object_create(1u, NULL, NULL);
	root  = cap_create(obj->cap_object_id, 10u, CAP_READ | CAP_DELEGATE, NULL, NULL);
	child = cap_create(obj->cap_object_id, 5u, CAP_READ, root, NULL);

	root->revoked = true;

	result = cap_is_authorized(10u, child);
	cr_assert_eq(result, CAP_REVOKED, "ancestor target should be blocked by revoked root");

	result = cap_is_authorized(5u, child);
	cr_assert_eq(result, CAP_OK, "direct target should still be authorized despite revoked ancestor");

	result = cap_is_authorized(99u, child);
	cr_assert_eq(result, CAP_REVOKED, "unauthorized caller should hit revoked ancestor");

	cap_destroy(child);
	cap_destroy(root);
	cap_object_destroy(obj);
}
