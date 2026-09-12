#include "../../kernel/src/capability/dma.h"

#include <base/address_space.h>
#include <base/dma.h>
#include <base/kernel_resource.h>
#include <base/memory.h>
#include <core/capability.h>
#include <core/dma.h>
#include <core/memory.h>

#include "../../kernel/src/capability/kernel_resource.h"
#include "../../kernel/src/capability/memory.h"
#include "test_support.h"

extern void   hal_iommu_mock_reset_discovered_controllers(void);
extern void   mock_cache_reset(void);
extern size_t mock_cache_dma_device_sync_count(void);
extern size_t mock_cache_dma_device_sync_bytes(void);

static cap_id_t dma_test_grant(struct kernel_capability_test_context* ctx) {
	hal_iommu_mock_reset_discovered_controllers();
	cr_assert(dma_init());
	cr_assert(kernel_capability_dma_init());
	cap_id_t cap = kernel_capability_dma_grant(process_pid(ctx->process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	return cap;
}

static dma_source_t resolve_source(cap_id_t dma_cap, struct kernel_capability_test_context* ctx, uint32_t local_id) {
	const struct dma_resolve_source_request request = {
		.header                      = {.op = DMA_OP_RESOLVE_SOURCE},
		.controller_register_address = 0x1000u,
		.local_source_id             = local_id,
		.reserved                    = 0u,
	};
	struct dma_resolve_source_response response;
	syscall_result_t                   result =
		kernel_capability_test_call(dma_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(response.source, DMA_SOURCE_INVALID);
	(void)ctx;
	return response.source;
}

Test(kernel_capability_dma, kernel_resource_lists_and_grants_dma_control) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/dma-resource");
	hal_iommu_mock_reset_discovered_controllers();
	cr_assert(dma_init());
	cr_assert(kernel_capability_dma_init());
	kernel_capability_resources_init();

	cap_id_t resources_cap = kernel_capability_resources_grant(process_pid(ctx.process));
	cr_assert_neq(resources_cap, CAP_ID_INVALID);

	struct kernel_resources_list_request list_request = {
		.header = {.op = KERNEL_RESOURCES_OP_LIST}, .offset = 0u, .capacity = 4u};
	uint8_t storage[sizeof(struct kernel_resources_list_response) + 4u * sizeof(enum kernel_resource_type)];
	struct kernel_resources_list_response* list = (void*)storage;
	syscall_result_t                       result =
		kernel_capability_test_call(resources_cap, &list_request, sizeof(list_request), list, sizeof(storage));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(list->total, 1u);
	cr_assert_eq(list->returned, 1u);
	cr_assert_eq(list->ids[0], KERNEL_RESOURCE_TYPE_DMA);

	const struct kernel_resource_acquire_request acquire_request = {.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
	                                                                .id     = KERNEL_RESOURCE_TYPE_DMA};
	struct kernel_resource_acquire_response      acquire_response;
	result = kernel_capability_test_call(
		resources_cap, &acquire_request, sizeof(acquire_request), &acquire_response, sizeof(acquire_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	struct capability* acquired = cap_acquire(acquire_response.cap);
	cr_assert_not_null(acquired);
	cr_assert_eq(cap_rights(acquired), CAP_CALL | CAP_READ | CAP_ALLOCATE | CAP_MANAGE | CAP_DELEGATE);
	cap_release(acquired);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_dma, device_resources_map_sync_bind_recover_and_unbind) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/dma-lifecycle");
	cap_id_t     dma_cap = dma_test_grant(&ctx);
	dma_source_t source  = resolve_source(dma_cap, &ctx, 3u);

	const struct dma_create_address_space_request create_request = {.header = {.op = DMA_OP_CREATE_ADDRESS_SPACE},
	                                                                .source = source};
	struct dma_create_address_space_response      create_response;
	syscall_result_t                              result = kernel_capability_test_call(
        dma_cap, &create_request, sizeof(create_request), &create_response, sizeof(create_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(create_response.address_space_cap, CAP_ID_INVALID);

	const struct address_space_info_request info_request = {.header = {.op = ADDRESS_SPACE_OP_INFO}};
	struct address_space_info               info;
	result = kernel_capability_test_call(
		create_response.address_space_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.kind, ADDRESS_SPACE_KIND_DEVICE);
	cr_assert_neq(info.minimum_mapping_size, 0u);

	struct memory* memory;
	cr_assert(memory_create_anonymous(2u * info.minimum_mapping_size, &memory));
	cap_id_t memory_cap =
		kernel_memory_grant(memory, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE);
	memory_release(memory);
	cr_assert_neq(memory_cap, CAP_ID_INVALID);

	const struct address_space_map_request map_request = {
		.header       = {.op = ADDRESS_SPACE_OP_MAP},
		.memory_cap   = memory_cap,
		.access       = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
		.address      = 0u,
		.alignment    = 0u,
		.guard_before = 0u,
		.guard_after  = 0u,
	};
	struct address_space_map_response map_response;
	result = kernel_capability_test_call(
		create_response.address_space_cap, &map_request, sizeof(map_request), &map_response, sizeof(map_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(map_response.mapping_cap, CAP_ID_INVALID);

	mock_cache_reset();
	const struct mapping_sync_request sync_request = {
		.header   = {.op = MAPPING_OP_SYNC},
		.target   = DMA_SYNC_FOR_DEVICE,
		.reserved = 0u,
		.offset   = info.minimum_mapping_size,
		.size     = info.minimum_mapping_size,
	};
	result = kernel_capability_test_call(map_response.mapping_cap, &sync_request, sizeof(sync_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(mock_cache_dma_device_sync_count(), 1u);
	cr_assert_eq(mock_cache_dma_device_sync_bytes(), info.minimum_mapping_size);

	const struct dma_bind_request bind_request = {
		.header            = {.op = DMA_OP_BIND},
		.source            = source,
		.address_space_cap = create_response.address_space_cap,
	};
	struct dma_bind_response bind_response;
	result = kernel_capability_test_call(
		dma_cap, &bind_request, sizeof(bind_request), &bind_response, sizeof(bind_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(bind_response.binding_cap, CAP_ID_INVALID);

	/* Mapping and binding resources keep the DEVICE AddressSpace alive after its direct grant is lost. */
	cr_assert(cap_destroy_by_id(create_response.address_space_cap));
	cr_assert(cap_destroy_by_id(bind_response.binding_cap));

	const struct dma_recover_request recover_request = {.header = {.op = DMA_OP_RECOVER}, .source = source};
	struct dma_recover_response      recover_response;
	result = kernel_capability_test_call(
		dma_cap, &recover_request, sizeof(recover_request), &recover_response, sizeof(recover_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(recover_response.binding_cap, CAP_ID_INVALID);

	const struct dma_binding_unbind_request unbind_request = {.header = {.op = DMA_BINDING_OP_UNBIND}};
	result =
		kernel_capability_test_call(recover_response.binding_cap, &unbind_request, sizeof(unbind_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = kernel_capability_test_call(
		dma_cap, &recover_request, sizeof(recover_request), &recover_response, sizeof(recover_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	const struct mapping_unmap_request unmap_request = {.header = {.op = MAPPING_OP_UNMAP}};
	result = kernel_capability_test_call(map_response.mapping_cap, &unmap_request, sizeof(unmap_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_dma, source_tokens_do_not_bypass_dma_capability_rights) {
	struct kernel_capability_test_context ctx;
	kernel_capability_test_begin(&ctx, "kernel-cap/dma-rights");
	cap_id_t dma_cap = dma_test_grant(&ctx);

	struct capability* root = cap_acquire(dma_cap);
	cr_assert_not_null(root);
	cap_id_t read_only = cap_delegate_create(root, process_pid(ctx.process), CAP_CALL | CAP_READ, false);
	cap_release(root);
	cr_assert_neq(read_only, CAP_ID_INVALID);

	dma_source_t source = resolve_source(read_only, &ctx, 7u);

	const struct dma_create_address_space_request create_request = {.header = {.op = DMA_OP_CREATE_ADDRESS_SPACE},
	                                                                .source = source};
	struct dma_create_address_space_response      create_response;
	syscall_result_t                              result = kernel_capability_test_call(
        read_only, &create_request, sizeof(create_request), &create_response, sizeof(create_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);

	const struct dma_recover_request recover_request = {.header = {.op = DMA_OP_RECOVER}, .source = source};
	struct dma_recover_response      recover_response;
	result = kernel_capability_test_call(
		read_only, &recover_request, sizeof(recover_request), &recover_response, sizeof(recover_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);

	kernel_capability_test_end(&ctx);
}
