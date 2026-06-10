#include <base/cap.h>
#include <base/heap.h>
#include <base/process.h>
#include <core/capability.h>
#include <core/channel.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KiB(x) ((size_t)(x) * 1024u)
#define CAP_TEST_HEAP_SIZE KiB(128)

static uint8_t cap_test_heap[CAP_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  cap_test_heap_offset;
static bool    cap_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&cap_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > CAP_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&cap_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = cap_test_heap + offset;
			return true;
		}
	}
}

static void cap_test_setup(void) {
	if (!cap_test_heap_initialized) {
		cap_test_heap_offset = 0u;
		cr_assert(heap_init(), "heap_init failed");
		cap_test_heap_initialized = true;
	}
	capability_init();
}

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
	cr_assert_neq(obj2, obj1, "cap_object_create should create distinct objects (no dedup in core helper)");

	found = cap_object_lookup(NULL, 42u);
	cr_assert_eq(found, obj1, "cap_object_lookup should return the first match");

	found = cap_object_lookup(NULL, 99u);
	cr_assert_null(found, "cap_object_lookup should return NULL for non-existent object");

	cap_object_destroy(obj1);
	cap_object_destroy(obj2);
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

Test(capability, cap_create_rejects_null_object) {
	struct capability* cap;

	cap_test_setup();

	cap = cap_create(NULL, 1u, CAP_READ, NULL);
	cr_assert_null(cap, "cap_create should reject NULL object");
}

Test(capability, cap_create_lookup_destroy) {
	struct cap_object* obj;
	struct capability* cap;
	struct capability* found;

	cap_test_setup();

	obj = cap_object_create(10u, NULL);
	cr_assert_not_null(obj);

	cap = cap_create(obj, 5u, CAP_READ | CAP_WRITE, NULL);
	cr_assert_not_null(cap, "cap_create should succeed");
	cr_assert_neq(cap->cap_id, CAP_ID_INVALID, "cap_id should be valid");
	cr_assert_eq(cap->object, obj, "cap object mismatch");
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

Test(capability, authorization_direct_target) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	obj = cap_object_create(1u, NULL);
	cap = cap_create(obj, 5u, CAP_READ, NULL);

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

	obj = cap_object_create(1u, ch);
	cap = cap_create(obj, 5u, CAP_READ, NULL);

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

	obj = cap_object_create(1u, NULL);

	root       = cap_create(obj, 10u, CAP_READ | CAP_DELEGATE, NULL);
	child      = cap_create(obj, 5u, CAP_READ, root);
	grandchild = cap_create(obj, 7u, CAP_READ, child);

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

	obj = cap_object_create(1u, NULL);
	cap = cap_create(obj, 5u, CAP_READ, NULL);

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

	obj   = cap_object_create(1u, NULL);
	root  = cap_create(obj, 10u, CAP_READ | CAP_DELEGATE, NULL);
	child = cap_create(obj, 5u, CAP_READ, root);

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

Test(capability, validity_revoked_self) {
	struct cap_object* obj;
	struct capability* cap;
	enum cap_result    result;

	cap_test_setup();

	obj = cap_object_create(1u, NULL);
	cap = cap_create(obj, 5u, CAP_READ, NULL);

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

	obj        = cap_object_create(1u, NULL);
	root       = cap_create(obj, 10u, CAP_READ | CAP_DELEGATE, NULL);
	child      = cap_create(obj, 5u, CAP_READ, root);
	grandchild = cap_create(obj, 7u, CAP_READ, child);

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

Test(capability, deep_parent_chain) {
	struct cap_object* obj;
	struct capability* caps[8];
	enum cap_result    result;
	process_id_t       targets[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	cap_test_setup();

	obj     = cap_object_create(1u, NULL);
	caps[0] = cap_create(obj, targets[0], CAP_READ | CAP_DELEGATE, NULL);
	cr_assert_not_null(caps[0]);

	for (size_t i = 1; i < 8; i++) {
		caps[i] = cap_create(obj, targets[i], CAP_READ, caps[i - 1]);
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
		caps[i] = cap_create(objs[i], 1u, CAP_READ, NULL);
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
	cap = cap_create(obj, 1u, CAP_READ, NULL);

	cr_assert_null(cap_lookup(999999u), "non-existent ID lookup should return NULL");

	cap_destroy(cap);
	cap_object_destroy(obj);
}
