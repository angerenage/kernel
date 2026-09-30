#include <base/hardware/pci.h>
#include <base/kernel_resource.h>
#include <base/record.h>
#include <core/capability.h>

#include "../../kernel/src/capability/kernel_resource.h"
#include "test_support.h"

static const struct pci_controller test_controllers[] = {
	{
     .register_address = 0xe0000000u,
     .register_size    = 128u * 1024u * 1024u,
     .access           = PCI_CONFIG_ACCESS_ECAM,
     .domain           = 0u,
     .start_bus        = 0u,
     .end_bus          = 127u,
	 },
	{
     .register_address = 0xf8000000u,
     .register_size    = 128u * 1024u * 1024u,
     .access           = PCI_CONFIG_ACCESS_ECAM,
     .domain           = 2u,
     .start_bus        = 128u,
     .end_bus          = 255u,
	 },
};

Test(kernel_capability_record, transports_client_typed_records_without_embedded_metadata) {
	struct kernel_capability_test_context   ctx;
	const struct record_count_request       count_request = {.header = {.op = RECORD_OP_COUNT}};
	struct record_count_response            count_response;
	struct record_get_request               get_request = {.header = {.op = RECORD_OP_GET}, .index = 1u};
	struct pci_controller                   controller;
	struct kernel_resource_acquire_request  acquire_request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                           .id     = KERNEL_RESOURCE_TYPE_PCI};
	struct kernel_resource_acquire_response acquire_response;
	struct capability*                      acquired;
	cap_id_t                                call_only_cap;
	cap_id_t                                resources_cap;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/records");
	hardware_mock_set_pci_controllers(test_controllers, sizeof(test_controllers) / sizeof(test_controllers[0]));
	cr_assert(kernel_capability_resources_init());
	resources_cap = kernel_capability_resources_grant(process_pid(ctx.process));
	cr_assert_neq(resources_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		resources_cap, &acquire_request, sizeof(acquire_request), &acquire_response, sizeof(acquire_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = kernel_capability_test_call(
		acquire_response.cap, &count_request, sizeof(count_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(count_response.count, 2u);
	result = kernel_capability_test_call(
		acquire_response.cap, &get_request, sizeof(get_request), &controller, sizeof(controller));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(controller));
	cr_assert_eq(controller.register_address, 0xf8000000u);
	cr_assert_eq(controller.register_size, 128u * 1024u * 1024u);
	cr_assert_eq(controller.access, PCI_CONFIG_ACCESS_ECAM);
	cr_assert_eq(controller.domain, 2u);
	cr_assert_eq(controller.start_bus, 128u);
	cr_assert_eq(controller.end_bus, 255u);

	get_request.index = 2u;
	result            = kernel_capability_test_call(
		acquire_response.cap, &get_request, sizeof(get_request), &controller, sizeof(controller));
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	result = kernel_capability_test_call(
		acquire_response.cap, &get_request, sizeof(get_request), &controller, sizeof(controller) - 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	acquired = cap_acquire(acquire_response.cap);
	cr_assert_not_null(acquired);
	call_only_cap = cap_create(acquired->cap_object_id, process_pid(ctx.process), CAP_CALL, acquired);
	cap_release(acquired);
	cr_assert_neq(call_only_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		call_only_cap, &count_request, sizeof(count_request), &count_response, sizeof(count_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_record, exposes_pci_as_a_separate_read_only_kernel_resource) {
	struct kernel_capability_test_context      ctx;
	const struct kernel_resources_list_request list_request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 1u};
	uint8_t list_storage[sizeof(struct kernel_resources_list_response) + sizeof(enum kernel_resource_type)];
	struct kernel_resources_list_response*       list_response   = (void*)list_storage;
	const struct kernel_resource_acquire_request acquire_request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                                .id     = KERNEL_RESOURCE_TYPE_PCI};
	struct kernel_resource_acquire_response      acquire_response;
	struct capability*                           acquired;
	cap_id_t                                     resources_cap;
	syscall_result_t                             result;

	kernel_capability_test_begin(&ctx, "kernel-cap/pci-resource");
	hardware_mock_set_pci_controllers(test_controllers, sizeof(test_controllers) / sizeof(test_controllers[0]));
	cr_assert(kernel_capability_resources_init());
	resources_cap = kernel_capability_resources_grant(process_pid(ctx.process));
	cr_assert_neq(resources_cap, CAP_ID_INVALID);

	result = kernel_capability_test_call(
		resources_cap, &list_request, sizeof(list_request), list_response, sizeof(list_storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list_response->total, 2u);
	cr_assert_eq(list_response->returned, 1u);
	cr_assert_eq(list_response->ids[0], KERNEL_RESOURCE_TYPE_PCI);

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
