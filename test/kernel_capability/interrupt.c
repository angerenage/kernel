#include "../../kernel/src/capability/interrupt.h"

#include <base/interrupt.h>
#include <base/kernel_resource.h>
#include <core/capability.h>
#include <core/interrupt.h>
#include <core/signal.h>

#include "../../kernel/src/capability/kernel_resource.h"
#include "../../kernel/src/capability/signal.h"
#include "test_support.h"

extern void hal_interrupt_mock_set_mask_failure(bool fail);

static cap_id_t interrupt_test_resource_grant(struct kernel_capability_test_context* ctx) {
	cr_assert(interrupt_init());
	cr_assert(kernel_capability_interrupts_init());
	cap_id_t cap = kernel_capability_interrupts_grant(process_pid(ctx->process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	return cap;
}

static interrupt_source_t interrupt_test_source(uint32_t number) {
	interrupt_source_t token;
	cr_assert_eq(interrupt_resolve_source(0x10000000u, number, &token), INTERRUPT_OK);
	return token;
}

static interrupt_message_context_t interrupt_test_resolve_message(cap_id_t resource_cap) {
	const struct interrupts_resolve_message_context_request request = {
		.header                      = {.op = INTERRUPTS_OP_RESOLVE_MESSAGE_CONTEXT},
		.controller_register_address = INTERRUPT_MESSAGE_CONTROLLER_AUTO,
		.producer_id                 = INTERRUPT_MESSAGE_PRODUCER_NONE,
		.reserved                    = 0u,
	};
	struct interrupts_resolve_message_context_response response = {0};
	syscall_result_t                                   result =
		kernel_capability_test_call(resource_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response));
	cr_assert_neq(response.context, INTERRUPT_MESSAGE_CONTEXT_INVALID);
	return response.context;
}

static cap_id_t interrupt_test_claim_config(cap_id_t resource_cap, interrupt_source_t source,
                                            enum interrupt_trigger trigger, enum interrupt_polarity polarity) {
	const struct interrupts_claim_source_request request = {
		.header   = {.op = INTERRUPTS_OP_CLAIM_SOURCE},
		.source   = source,
		.trigger  = trigger,
		.polarity = polarity,
	};
	struct interrupts_claim_source_response response = {0};
	syscall_result_t                        result =
		kernel_capability_test_call(resource_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response));
	cr_assert_neq(response.interrupt_cap, CAP_ID_INVALID);
	return response.interrupt_cap;
}

static cap_id_t interrupt_test_claim(cap_id_t resource_cap, interrupt_source_t source) {
	return interrupt_test_claim_config(resource_cap, source, INTERRUPT_TRIGGER_EDGE, INTERRUPT_POLARITY_HIGH);
}

static syscall_result_t interrupt_test_try_claim_config(cap_id_t resource_cap, interrupt_source_t source,
                                                        enum interrupt_trigger                   trigger,
                                                        enum interrupt_polarity                  polarity,
                                                        struct interrupts_claim_source_response* response) {
	const struct interrupts_claim_source_request request = {
		.header   = {.op = INTERRUPTS_OP_CLAIM_SOURCE},
		.source   = source,
		.trigger  = trigger,
		.polarity = polarity,
	};
	return kernel_capability_test_call(resource_cap, &request, sizeof(request), response, sizeof(*response));
}

static syscall_result_t interrupt_test_try_claim(cap_id_t resource_cap, interrupt_source_t source,
                                                 struct interrupts_claim_source_response* response) {
	return interrupt_test_try_claim_config(
		resource_cap, source, INTERRUPT_TRIGGER_EDGE, INTERRUPT_POLARITY_HIGH, response);
}

static syscall_result_t interrupt_test_simple_call(cap_id_t cap, enum interrupt_op op) {
	const struct interrupt_simple_request request = {.header = {.op = op}};
	return kernel_capability_test_call(cap, &request, sizeof(request), NULL, 0u);
}

Test(kernel_capability_interrupt, resource_is_listed_and_granted_with_expected_rights) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-resource");
	(void)interrupt_test_resource_grant(&ctx);
	kernel_capability_resources_init();

	cap_id_t resources_cap = kernel_capability_resources_grant(process_pid(ctx.process));
	cr_assert_neq(resources_cap, CAP_ID_INVALID);
	const struct kernel_resources_list_request list_request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 8u};
	uint8_t storage[sizeof(struct kernel_resources_list_response) + 8u * sizeof(enum kernel_resource_type)] = {0};
	struct kernel_resources_list_response* list = (void*)storage;
	syscall_result_t                       result =
		kernel_capability_test_call(resources_cap, &list_request, sizeof(list_request), list, sizeof(storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list->total, 1u);
	cr_assert_eq(list->returned, 1u);
	cr_assert_eq(list->ids[0], KERNEL_RESOURCE_TYPE_INTERRUPTS);

	const struct kernel_resource_acquire_request acquire_request  = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                                 .id     = KERNEL_RESOURCE_TYPE_INTERRUPTS};
	struct kernel_resource_acquire_response      acquire_response = {0};
	result                                                        = kernel_capability_test_call(
		resources_cap, &acquire_request, sizeof(acquire_request), &acquire_response, sizeof(acquire_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	struct capability* acquired = cap_acquire(acquire_response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_READ | CAP_MANAGE | CAP_ALLOCATE | CAP_DELEGATE);
	cap_release(acquired);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, source_lifecycle_preserves_capabilities_when_destroy_fails) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-source");
	cap_id_t resource_cap  = interrupt_test_resource_grant(&ctx);
	cap_id_t interrupt_cap = interrupt_test_claim(resource_cap, interrupt_test_source(40u));

	struct capability* interrupt_grant = cap_acquire(interrupt_cap);
	cr_assert_not_null(interrupt_grant);
	cr_assert_eq(cap_rights(interrupt_grant), CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DESTROY | CAP_DELEGATE);
	cap_id_t delegated = cap_delegate_create(
		interrupt_grant, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DESTROY, false);
	cap_release(interrupt_grant);
	cr_assert_neq(delegated, CAP_ID_INVALID);

	struct signal* signal = signal_create();
	cr_assert_not_null(signal);
	cap_id_t signal_cap = kernel_signal_grant_full(signal, process_pid(ctx.process));
	cr_assert_neq(signal_cap, CAP_ID_INVALID);
	const struct interrupt_bind_request bind_request = {.header = {.op = INTERRUPT_OP_BIND}, .signal_cap = signal_cap};
	syscall_result_t result = kernel_capability_test_call(interrupt_cap, &bind_request, sizeof(bind_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	hal_interrupt_mock_set_mask_failure(true);
	result = interrupt_test_simple_call(interrupt_cap, INTERRUPT_OP_DESTROY);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	struct interrupt_info_response response;
	memset(&response, 0xa5, sizeof(response));
	const struct interrupt_simple_request info_request = {.header = {.op = INTERRUPT_OP_INFO}};
	result = kernel_capability_test_call(delegated, &info_request, sizeof(info_request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	struct interrupt_info_response expected = {0};
	expected.info.kind                      = INTERRUPT_KIND_SOURCE;
	expected.info.bound                     = true;
	cr_assert_eq(memcmp(&response, &expected, sizeof(response)), 0);

	hal_interrupt_mock_set_mask_failure(false);
	result = interrupt_test_simple_call(delegated, INTERRUPT_OP_DESTROY);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_acquire(interrupt_cap));
	cr_assert_null(cap_acquire(delegated));
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_release(signal);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, successful_zero_grants_destroy_releases_source) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-zero-grants");
	cap_id_t           resource_cap = interrupt_test_resource_grant(&ctx);
	interrupt_source_t source       = interrupt_test_source(41u);
	cap_id_t           first        = interrupt_test_claim(resource_cap, source);
	cr_assert(cap_destroy_by_id(first));
	cap_id_t second = interrupt_test_claim(resource_cap, source);
	cr_assert_eq(interrupt_test_simple_call(second, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, failed_zero_grants_destroy_quarantines_source) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-zero-grants-failure");
	cap_id_t           resource_cap  = interrupt_test_resource_grant(&ctx);
	interrupt_source_t source        = interrupt_test_source(43u);
	cap_id_t           interrupt_cap = interrupt_test_claim(resource_cap, source);

	hal_interrupt_mock_set_mask_failure(true);
	cr_assert(cap_destroy_by_id(interrupt_cap));
	cr_assert_null(cap_acquire(interrupt_cap));
	struct interrupts_claim_source_response response = {0};
	syscall_result_t                        result   = interrupt_test_try_claim(resource_cap, source, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, INTERRUPT_ALREADY_CLAIMED);
	cr_assert_eq(response.interrupt_cap, CAP_ID_INVALID);
	hal_interrupt_mock_set_mask_failure(false);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, response_rollback_failure_removes_grant_and_quarantines_source) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-response-rollback-failure");
	cap_id_t                                resource_cap = interrupt_test_resource_grant(&ctx);
	interrupt_source_t                      source       = interrupt_test_source(44u);
	struct interrupts_claim_source_response response     = {0};

	hal_interrupt_mock_set_mask_failure(true);
	kernel_capability_interrupt_test_fail_next_response();
	syscall_result_t result = interrupt_test_try_claim(resource_cap, source, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(response.interrupt_cap, CAP_ID_INVALID);
	cap_id_t rolled_back = kernel_capability_interrupt_test_last_rollback_cap();
	cr_assert_neq(rolled_back, CAP_ID_INVALID);
	cr_assert_null(cap_acquire(rolled_back));

	result = interrupt_test_try_claim(resource_cap, source, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, INTERRUPT_ALREADY_CLAIMED);
	cr_assert_eq(response.interrupt_cap, CAP_ID_INVALID);
	hal_interrupt_mock_set_mask_failure(false);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, message_allocation_zero_initializes_response) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-message");
	cap_id_t resource_cap = interrupt_test_resource_grant(&ctx);

	interrupt_message_context_t                      context = interrupt_test_resolve_message(resource_cap);
	const struct interrupts_allocate_message_request request = {.header  = {.op = INTERRUPTS_OP_ALLOCATE_MESSAGE},
	                                                            .context = context};
	struct interrupts_allocate_message_response      response;
	memset(&response, 0xa5, sizeof(response));
	syscall_result_t result =
		kernel_capability_test_call(resource_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(response.interrupt_cap, CAP_ID_INVALID);
	struct interrupts_allocate_message_response expected = {0};
	expected.interrupt_cap                               = response.interrupt_cap;
	expected.message_address                             = response.message_address;
	expected.message_data                                = response.message_data;
	cr_assert_eq(memcmp(&response, &expected, sizeof(response)), 0);
	cr_assert_eq(interrupt_test_simple_call(response.interrupt_cap, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, resolution_is_stable_stateless_and_read_only) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-resolution");
	cap_id_t                                       resource_cap   = interrupt_test_resource_grant(&ctx);
	const struct interrupts_resolve_source_request source_request = {
		.header                      = {.op = INTERRUPTS_OP_RESOLVE_SOURCE},
		.controller_register_address = 0x10000000u,
		.local_source_id             = 46u,
	};
	struct interrupts_resolve_source_response first  = {0};
	struct interrupts_resolve_source_response second = {0};
	syscall_result_t                          result =
		kernel_capability_test_call(resource_cap, &source_request, sizeof(source_request), &first, sizeof(first));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	result =
		kernel_capability_test_call(resource_cap, &source_request, sizeof(source_request), &second, sizeof(second));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(first.source, second.source);

	cap_id_t configured =
		interrupt_test_claim_config(resource_cap, first.source, INTERRUPT_TRIGGER_LEVEL, INTERRUPT_POLARITY_LOW);
	cr_assert_eq(interrupt_test_simple_call(configured, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);
	configured =
		interrupt_test_claim_config(resource_cap, first.source, INTERRUPT_TRIGGER_EDGE, INTERRUPT_POLARITY_HIGH);
	cr_assert_eq(interrupt_test_simple_call(configured, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);

	struct interrupts_resolve_source_request unknown = source_request;
	unknown.controller_register_address              = 0x20000000u;
	result = kernel_capability_test_call(resource_cap, &unknown, sizeof(unknown), &second, sizeof(second));
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, INTERRUPT_NOT_FOUND);

	struct interrupts_resolve_message_context_request invalid_message = {
		.header                      = {.op = INTERRUPTS_OP_RESOLVE_MESSAGE_CONTEXT},
		.controller_register_address = INTERRUPT_MESSAGE_CONTROLLER_AUTO,
		.producer_id                 = INTERRUPT_MESSAGE_PRODUCER_NONE,
		.reserved                    = 1u,
	};
	struct interrupts_resolve_message_context_response message_response = {0};
	result                                                              = kernel_capability_test_call(
		resource_cap, &invalid_message, sizeof(invalid_message), &message_response, sizeof(message_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, core_results_map_to_public_syscall_statuses) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-result-mapping");
	cap_id_t                                resource_cap  = interrupt_test_resource_grant(&ctx);
	interrupt_source_t                      source        = interrupt_test_source(45u);
	cap_id_t                                interrupt_cap = interrupt_test_claim(resource_cap, source);
	struct interrupts_claim_source_response response      = {0};

	syscall_result_t result = interrupt_test_try_claim(resource_cap, source, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, INTERRUPT_ALREADY_CLAIMED);

	result = interrupt_test_try_claim(resource_cap, UINT64_MAX - 1u, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, INTERRUPT_NOT_FOUND);

	result = interrupt_test_simple_call(interrupt_cap, INTERRUPT_OP_UNBIND);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, INTERRUPT_NOT_BOUND);
	cr_assert_eq(interrupt_test_simple_call(interrupt_cap, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_interrupt, operations_enforce_capability_rights) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/interrupt-rights");
	cap_id_t           resource_cap   = interrupt_test_resource_grant(&ctx);
	struct capability* resource_grant = cap_acquire(resource_cap);
	cr_assert_not_null(resource_grant);
	cap_id_t call_only = cap_delegate_create(resource_grant, process_pid(ctx.process), CAP_CALL, false);
	cap_release(resource_grant);
	cr_assert_neq(call_only, CAP_ID_INVALID);
	const struct interrupts_resolve_source_request resolve_request = {
		.header                      = {.op = INTERRUPTS_OP_RESOLVE_SOURCE},
		.controller_register_address = 0x10000000u,
		.local_source_id             = 42u,
	};
	struct interrupts_resolve_source_response resolve_response = {0};
	syscall_result_t                          result           = kernel_capability_test_call(
		call_only, &resolve_request, sizeof(resolve_request), &resolve_response, sizeof(resolve_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	resource_grant = cap_acquire(resource_cap);
	cr_assert_not_null(resource_grant);
	cap_id_t read_only = cap_delegate_create(resource_grant, process_pid(ctx.process), CAP_CALL | CAP_READ, false);
	cap_release(resource_grant);
	cr_assert_neq(read_only, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		read_only, &resolve_request, sizeof(resolve_request), &resolve_response, sizeof(resolve_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(resolve_response.source, INTERRUPT_SOURCE_INVALID);

	resource_grant = cap_acquire(resource_cap);
	cr_assert_not_null(resource_grant);
	cap_id_t manage_only = cap_delegate_create(resource_grant, process_pid(ctx.process), CAP_CALL | CAP_MANAGE, false);
	cap_release(resource_grant);
	cr_assert_neq(manage_only, CAP_ID_INVALID);
	result = kernel_capability_test_call(
		manage_only, &resolve_request, sizeof(resolve_request), &resolve_response, sizeof(resolve_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);

	const struct interrupts_claim_source_request claim_request = {
		.header   = {.op = INTERRUPTS_OP_CLAIM_SOURCE},
		.source   = interrupt_test_source(42u),
		.trigger  = INTERRUPT_TRIGGER_EDGE,
		.polarity = INTERRUPT_POLARITY_HIGH,
	};
	struct interrupts_claim_source_response claim_response = {0};
	result                                                 = kernel_capability_test_call(
		call_only, &claim_request, sizeof(claim_request), &claim_response, sizeof(claim_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	interrupt_message_context_t                      context          = interrupt_test_resolve_message(resource_cap);
	const struct interrupts_allocate_message_request allocate_request = {
		.header = {.op = INTERRUPTS_OP_ALLOCATE_MESSAGE}, .context = context};
	struct interrupts_allocate_message_response allocate_response = {0};
	result                                                        = kernel_capability_test_call(
		call_only, &allocate_request, sizeof(allocate_request), &allocate_response, sizeof(allocate_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);

	cap_id_t           interrupt_cap   = interrupt_test_claim(resource_cap, claim_request.source);
	struct capability* interrupt_grant = cap_acquire(interrupt_cap);
	cr_assert_not_null(interrupt_grant);
	cap_id_t interrupt_call_only = cap_delegate_create(interrupt_grant, process_pid(ctx.process), CAP_CALL, false);
	cap_release(interrupt_grant);
	cr_assert_neq(interrupt_call_only, CAP_ID_INVALID);
	struct interrupt_info_response        info         = {0};
	const struct interrupt_simple_request info_request = {.header = {.op = INTERRUPT_OP_INFO}};
	result = kernel_capability_test_call(interrupt_call_only, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(interrupt_test_simple_call(interrupt_call_only, INTERRUPT_OP_UNBIND).status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(interrupt_test_simple_call(interrupt_call_only, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_DENIED);

	struct signal* signal = signal_create();
	cr_assert_not_null(signal);
	cap_id_t signal_without_send = kernel_signal_grant(signal, process_pid(ctx.process), CAP_CALL | CAP_DELEGATE);
	cr_assert_neq(signal_without_send, CAP_ID_INVALID);
	const struct interrupt_bind_request bind_request = {.header     = {.op = INTERRUPT_OP_BIND},
	                                                    .signal_cap = signal_without_send};
	result = kernel_capability_test_call(interrupt_cap, &bind_request, sizeof(bind_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(interrupt_test_simple_call(interrupt_cap, INTERRUPT_OP_DESTROY).status, SYSCALL_STATUS_OK);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_release(signal);

	kernel_capability_test_end(&ctx);
}
