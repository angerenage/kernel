#include "../../kernel/src/capability/kernel_resource.h"

#include <base/kernel_resource.h>

#include "../../kernel/src/capability/boot_module.h"
#include "../../kernel/src/capability/boot_resource.h"
#include "../../kernel/src/capability/loader.h"
#include "../../kernel/src/capability/serial.h"
#include "test_support.h"

static cap_id_t init_resources(struct kernel_capability_test_context* ctx) {
	kernel_capability_boot_module_provider_init();
	kernel_capability_boot_resources_init();
	kernel_capability_serial_init();
	kernel_capability_loader_init();
	kernel_capability_resources_init();
	return kernel_capability_resources_grant(process_pid(ctx->process));
}

Test(kernel_capability_resource, framebuffer_is_listed_only_when_available) {
	struct kernel_capability_test_context ctx;
	static uint8_t                        framebuffer_bytes[64];
	const struct kernel_boot_framebuffer  framebuffer = {
		 .address = framebuffer_bytes, .width = 4u, .height = 4u, .pitch = 16u, .bpp = 32u};
	struct kernel_resources_list_request request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 4u};
	uint8_t response_storage[sizeof(struct kernel_resources_list_response) + 4u * sizeof(enum kernel_resource_type)];
	struct kernel_resources_list_response* response = (void*)response_storage;
	cap_id_t                               root_cap;
	syscall_result_t                       result;

	kernel_capability_test_begin(&ctx, "kernel-cap/resources-framebuffer");
	kernel_boot_mock_set_framebuffer(&framebuffer);
	root_cap = init_resources(&ctx);
	cr_assert_neq(root_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(root_cap, &request, sizeof(request), response, sizeof(response_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response->total, 3u);
	cr_assert_eq(response->returned, 3u);
	cr_assert_eq(response->ids[2], KERNEL_RESOURCE_TYPE_FRAMEBUFFER);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_resource, empty_and_zero_capacity_lists_report_totals) {
	struct kernel_capability_test_context ctx;
	const struct kernel_boot_module       modules[] = {
        {.name = "one.bin", .size = 1u}
    };
	struct kernel_resources_list_request request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 0u};
	struct kernel_resources_list_response response;
	cap_id_t                              root_cap;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/resources-list");
	root_cap = init_resources(&ctx);
	cr_assert_neq(root_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response.total, 2u);
	cr_assert_eq(response.returned, 0u);

	kernel_boot_mock_set_modules(modules, 1u);
	result = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response.total, 3u);
	cr_assert_eq(response.returned, 0u);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_resource, serial_and_loader_are_acquired_from_the_registry) {
	struct kernel_capability_test_context   ctx;
	struct kernel_resource_acquire_request  request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE}};
	struct kernel_resource_acquire_response response;
	struct capability*                      acquired;
	cap_id_t                                root_cap;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/resources-core-providers");
	root_cap = init_resources(&ctx);
	cr_assert_neq(root_cap, CAP_ID_INVALID);
	request.id = KERNEL_RESOURCE_TYPE_SERIAL;
	result     = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	acquired = cap_acquire(response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_WRITE | CAP_DELEGATE | CAP_DELEGATE_PEER);
	cap_release(acquired);

	request.id = KERNEL_RESOURCE_TYPE_LOADER;
	result     = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	acquired = cap_acquire(response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_DELEGATE);
	cap_release(acquired);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_resource, listing_is_paginated_and_acquisition_targets_the_caller) {
	struct kernel_capability_test_context ctx;
	const struct kernel_boot_module       modules[] = {
        {.name = "one.bin", .size = 1u}
    };
	struct kernel_resources_list_request list_request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 1u};
	uint8_t list_storage[sizeof(struct kernel_resources_list_response) + sizeof(enum kernel_resource_type)];
	struct kernel_resources_list_response*       list_response   = (void*)list_storage;
	const struct kernel_resource_acquire_request acquire_request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                                .id     = KERNEL_RESOURCE_TYPE_MODULES};
	struct kernel_resource_acquire_response      acquire_response;
	struct capability*                           acquired;
	cap_id_t                                     root_cap;
	syscall_result_t                             result;

	kernel_capability_test_begin(&ctx, "kernel-cap/resources-acquire");
	kernel_boot_mock_set_modules(modules, 1u);
	root_cap = init_resources(&ctx);
	cr_assert_neq(root_cap, CAP_ID_INVALID);
	result =
		kernel_capability_test_call(root_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list_response->total, 3u);
	cr_assert_eq(list_response->returned, 1u);
	cr_assert_eq(list_response->ids[0], KERNEL_RESOURCE_TYPE_MODULES);
	list_request.offset = 3u;
	result =
		kernel_capability_test_call(root_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list_response->total, 3u);
	cr_assert_eq(list_response->returned, 0u);

	result = kernel_capability_test_call(
		root_cap, &acquire_request, sizeof(acquire_request), &acquire_response, sizeof(acquire_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	acquired = cap_acquire(acquire_response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(acquired->target, process_pid(ctx.process));
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_READ | CAP_DELEGATE);
	cap_release(acquired);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_resource, unavailable_unknown_and_insufficient_rights_are_rejected_without_grants) {
	struct kernel_capability_test_context ctx;
	const struct kernel_boot_module       modules[] = {
        {.name = "one.bin", .size = 1u}
    };
	struct kernel_resource_acquire_request  request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                   .id     = KERNEL_RESOURCE_TYPE_MODULES};
	struct kernel_resource_acquire_response response;
	struct capability*                      root;
	cap_id_t                                root_cap;
	cap_id_t                                call_only_cap;
	size_t                                  grants_before;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/resources-errors");
	root_cap = init_resources(&ctx);
	cr_assert_neq(root_cap, CAP_ID_INVALID);
	grants_before = capability_count();
	result        = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(capability_count(), grants_before);

	request.id = 99u;
	result     = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), grants_before);

	kernel_boot_mock_set_modules(modules, 1u);
	request.id = KERNEL_RESOURCE_TYPE_MODULES;
	result     = kernel_capability_test_call(root_cap, &request, sizeof(request), &response, sizeof(response) - 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), grants_before);

	root = cap_acquire(root_cap);
	cr_assert_not_null(root);
	call_only_cap = cap_create(root->cap_object_id, process_pid(ctx.process), CAP_CALL, root);
	cap_release(root);
	cr_assert_neq(call_only_cap, CAP_ID_INVALID);
	request.id = KERNEL_RESOURCE_TYPE_MODULES;
	result     = kernel_capability_test_call(call_only_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}
