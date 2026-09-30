#include "../../kernel/src/capability/device.h"

#include <base/device.h>
#include <base/kernel_resource.h>
#include <core/capability.h>
#include <kernel/device.h>

#include "../../kernel/src/capability/kernel_resource.h"
#include "test_support.h"

#define TEST_DEVICE_TYPE ((enum kernel_device_type)0x70000001u)
#define TEST_OTHER_DEVICE_TYPE ((enum kernel_device_type)0x70000002u)

struct test_device_descriptor {
	uint64_t address;
	uint32_t kind;
	uint32_t flags;
};

static const struct test_device_descriptor test_devices[] = {
	{.address = 0x1000u, .kind = 1u, .flags = 0x10u},
	{.address = 0x2000u, .kind = 2u, .flags = 0x20u},
	{.address = 0x3000u, .kind = 3u, .flags = 0x30u},
};

static cap_id_t initialize_devices(struct kernel_capability_test_context* ctx) {
	cr_assert(kernel_capability_devices_init());
	cap_id_t cap = kernel_capability_devices_grant(process_pid(ctx->process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	return cap;
}

Test(kernel_capability_device, copies_and_paginates_typed_descriptors) {
	struct kernel_capability_test_context     ctx;
	struct test_device_descriptor             first         = test_devices[0];
	const struct kernel_devices_count_request count_request = {.header = {.op = KERNEL_DEVICES_OP_COUNT},
	                                                           .type   = TEST_DEVICE_TYPE};
	struct kernel_devices_count_response      count_response;
	struct kernel_devices_count_request       other_count_request = {.header = {.op = KERNEL_DEVICES_OP_COUNT},
	                                                                 .type   = TEST_OTHER_DEVICE_TYPE};
	struct kernel_devices_list_request        list_request        = {
		.header       = {.op = KERNEL_DEVICES_OP_LIST},
		.type         = TEST_DEVICE_TYPE,
		.offset       = 1u,
		.length       = 2u,
		.element_size = sizeof(struct test_device_descriptor),
	};
	uint8_t list_storage[sizeof(struct kernel_devices_list_response) + 2u * sizeof(struct test_device_descriptor)];
	struct kernel_devices_list_response* list_response = (void*)list_storage;
	struct test_device_descriptor*       listed        = (void*)list_response->entries;
	syscall_result_t                     result;
	cap_id_t                             devices_cap;

	kernel_capability_test_begin(&ctx, "kernel-cap/devices-list");
	cr_assert(kernel_device_register_type(TEST_DEVICE_TYPE, sizeof(first)));
	cr_assert(kernel_device_register_type(TEST_OTHER_DEVICE_TYPE, sizeof(first)));
	cr_assert(kernel_device_register(TEST_DEVICE_TYPE, &first, sizeof(first)));
	first.address = 0xffffu;
	cr_assert(kernel_device_register(TEST_DEVICE_TYPE, &test_devices[1], sizeof(test_devices[1])));
	cr_assert(kernel_device_register(TEST_DEVICE_TYPE, &test_devices[2], sizeof(test_devices[2])));
	devices_cap = initialize_devices(&ctx);

	result = kernel_capability_test_call(
		devices_cap, &count_request, sizeof(count_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(count_response.count, 3u);
	result = kernel_capability_test_call(
		devices_cap, &other_count_request, sizeof(other_count_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(count_response.count, 0u);
	result = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(list_storage));
	cr_assert_eq(list_response->returned, 2u);
	cr_assert_eq(listed[0].address, test_devices[1].address);
	cr_assert_eq(listed[0].kind, test_devices[1].kind);
	cr_assert_eq(listed[1].address, test_devices[2].address);
	cr_assert_eq(listed[1].flags, test_devices[2].flags);

	list_request.offset = 0u;
	list_request.length = 1u;
	result              = kernel_capability_test_call(devices_cap,
	                                                  &list_request,
	                                                  sizeof(list_request),
	                                                  list_response,
	                                                  sizeof(*list_response) + sizeof(struct test_device_descriptor));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list_response->returned, 1u);
	cr_assert_eq(listed[0].address, test_devices[0].address, "registered descriptors must be copied");

	list_request.offset = 99u;
	list_request.length = 2u;
	result              = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(*list_response));
	cr_assert_eq(list_response->returned, 0u);

	list_request.offset = 0u;
	list_request.length = 0u;
	result              = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_response, sizeof(*list_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(*list_response));
	cr_assert_eq(list_response->returned, 0u);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device, validates_registration_and_freezes_the_inventory) {
	struct kernel_capability_test_context ctx;
	struct test_device_descriptor         descriptor = test_devices[0];

	kernel_capability_test_begin(&ctx, "kernel-cap/devices-registration");
	cr_assert_not(kernel_device_register_type(KERNEL_DEVICE_TYPE_INVALID, sizeof(descriptor)));
	cr_assert_not(kernel_device_register_type(TEST_DEVICE_TYPE, 0u));
	cr_assert_not(kernel_device_register_type(
		TEST_DEVICE_TYPE, CAP_MAX_RESPONSE_SIZE - sizeof(struct kernel_devices_list_response) + 1u));
	cr_assert(kernel_device_register_type(TEST_DEVICE_TYPE, sizeof(descriptor)));
	cr_assert_not(kernel_device_register_type(TEST_DEVICE_TYPE, sizeof(descriptor)));
	cr_assert_not(kernel_device_register(TEST_OTHER_DEVICE_TYPE, &descriptor, sizeof(descriptor)));
	cr_assert_not(kernel_device_register(TEST_DEVICE_TYPE, NULL, sizeof(descriptor)));
	cr_assert_not(kernel_device_register(TEST_DEVICE_TYPE, &descriptor, sizeof(descriptor) - 1u));
	cr_assert(kernel_device_register(TEST_DEVICE_TYPE, &descriptor, sizeof(descriptor)));
	(void)initialize_devices(&ctx);
	cr_assert_not(kernel_device_register_type(TEST_OTHER_DEVICE_TYPE, sizeof(descriptor)));
	cr_assert_not(kernel_device_register(TEST_DEVICE_TYPE, &descriptor, sizeof(descriptor)));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device, rejects_malformed_requests_and_missing_read_rights) {
	struct kernel_capability_test_context     ctx;
	const struct kernel_devices_count_request count_request   = {.header = {.op = KERNEL_DEVICES_OP_COUNT},
	                                                             .type   = TEST_DEVICE_TYPE};
	struct kernel_devices_count_request       unknown_request = {.header = {.op = KERNEL_DEVICES_OP_COUNT},
	                                                             .type   = TEST_OTHER_DEVICE_TYPE};
	struct kernel_devices_count_response      count_response;
	struct kernel_devices_list_request        list_request = {
		.header       = {.op = KERNEL_DEVICES_OP_LIST},
		.type         = TEST_DEVICE_TYPE,
		.offset       = 0u,
		.length       = 1u,
		.element_size = sizeof(struct test_device_descriptor),
	};
	uint8_t list_storage[sizeof(struct kernel_devices_list_response) + sizeof(struct test_device_descriptor)];
	struct capability* root;
	cap_id_t           devices_cap;
	cap_id_t           call_only_cap;
	syscall_result_t   result;

	kernel_capability_test_begin(&ctx, "kernel-cap/devices-errors");
	cr_assert(kernel_device_register_type(TEST_DEVICE_TYPE, sizeof(struct test_device_descriptor)));
	cr_assert(kernel_device_register(TEST_DEVICE_TYPE, &test_devices[0], sizeof(test_devices[0])));
	devices_cap = initialize_devices(&ctx);

	result = kernel_capability_test_call(
		devices_cap, &unknown_request, sizeof(unknown_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	unknown_request.type = KERNEL_DEVICE_TYPE_INVALID;
	result               = kernel_capability_test_call(
		devices_cap, &unknown_request, sizeof(unknown_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	result = kernel_capability_test_call(
		devices_cap, &count_request, sizeof(count_request) - 1u, &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	unknown_request.header.op = (enum kernel_devices_op)99u;
	result                    = kernel_capability_test_call(
		devices_cap, &unknown_request, sizeof(unknown_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	list_request.element_size--;
	result = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_storage, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	list_request.element_size = sizeof(struct test_device_descriptor);
	result                    = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_storage, sizeof(list_storage) - 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	list_request.length = UINT64_MAX;
	result              = kernel_capability_test_call(
		devices_cap, &list_request, sizeof(list_request), list_storage, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	root = cap_acquire(devices_cap);
	cr_assert_not_null(root);
	call_only_cap = cap_create(root->cap_object_id, process_pid(ctx.process), CAP_CALL, root);
	cap_release(root);
	cr_assert_neq(call_only_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		call_only_cap, &count_request, sizeof(count_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device, is_a_permanent_kernel_resource) {
	struct kernel_capability_test_context      ctx;
	const struct kernel_resources_list_request list_request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = KERNEL_RESOURCE_TYPE_COUNT};
	uint8_t                                list_storage[sizeof(struct kernel_resources_list_response) +
	                                                    KERNEL_RESOURCE_TYPE_COUNT * sizeof(enum kernel_resource_type)];
	struct kernel_resources_list_response* list_response         = (void*)list_storage;
	const struct kernel_resource_acquire_request acquire_request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                                .id     = KERNEL_RESOURCE_TYPE_DEVICES};
	struct kernel_resource_acquire_response      acquire_response;
	struct capability*                           acquired;
	cap_id_t                                     resources_cap;
	syscall_result_t                             result;
	bool                                         found = false;

	kernel_capability_test_begin(&ctx, "kernel-cap/devices-resource");
	cr_assert(kernel_capability_resources_init());
	resources_cap = kernel_capability_resources_grant(process_pid(ctx.process));
	cr_assert_neq(resources_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		resources_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	for (size_t index = 0u; index < list_response->returned; index++) {
		if (list_response->ids[index] == KERNEL_RESOURCE_TYPE_DEVICES) found = true;
	}
	cr_assert(found);

	result = kernel_capability_test_call(
		resources_cap, &acquire_request, sizeof(acquire_request), &acquire_response, sizeof(acquire_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	acquired = cap_acquire(acquire_response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(acquired->target, process_pid(ctx.process));
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_READ | CAP_DELEGATE);
	cap_release(acquired);
	kernel_capability_test_end(&ctx);
}
