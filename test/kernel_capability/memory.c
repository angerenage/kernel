#include "../../kernel/src/capability/memory.h"

#include <base/address_space.h>
#include <base/memory.h>
#include <test_memory.h>

#include "../../kernel/src/capability/address_space.h"
#include "../../kernel/src/capability/memory_allocator.h"
#include "test_support.h"

static cap_id_t root_allocator(struct kernel_capability_test_context* ctx) {
	cap_id_t cap = kernel_memory_allocator_grant_root(process_pid(ctx->process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	return cap;
}

static cap_id_t allocate_memory(cap_id_t allocator, size_t size) {
	const struct memory_allocator_alloc_request request = {
		.header = {.op = MEMORY_ALLOCATOR_OP_ALLOC},
		.size   = size,
	};
	struct memory_allocator_alloc_response response;
	syscall_result_t                       result =
		kernel_capability_test_call(allocator, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(response.memory_cap, CAP_ID_INVALID);
	return response.memory_cap;
}

static syscall_result_t claim_memory(cap_id_t allocator, uintptr_t address, size_t size, enum memory_type type,
                                     struct memory_allocator_claim_physical_response* out_response) {
	const struct memory_allocator_claim_physical_request request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_CLAIM_PHYSICAL},
		.physical_address = address,
		.size             = size,
		.memory_type      = type,
	};
	return kernel_capability_test_call(allocator, &request, sizeof(request), out_response, sizeof(*out_response));
}

Test(kernel_capability_memory, allocator_info_alloc_and_arbitrary_slice) {
	struct kernel_capability_test_context ctx;
	const size_t                          size = 3u * TEST_MAPPING_GRANULE + 17u;
	struct memory_allocator_info          allocator_info;
	struct memory_info                    info;
	cap_id_t                              allocator;
	cap_id_t                              memory;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-alloc");
	allocator                                                    = root_allocator(&ctx);
	const struct memory_allocator_info_request allocator_request = {
		.header = {.op = MEMORY_ALLOCATOR_OP_INFO},
	};
	result = kernel_capability_test_call(
		allocator, &allocator_request, sizeof(allocator_request), &allocator_info, sizeof(allocator_info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(allocator_info.claim_policy, MEMORY_ALLOCATOR_CLAIMS_UNRESTRICTED);
	cr_assert_eq(allocator_info.physical_claim_granule, pmm_info()->allocation_granule);

	memory                                        = allocate_memory(allocator, size);
	const struct memory_info_request info_request = {.header = {.op = MEMORY_OP_INFO}};
	result = kernel_capability_test_call(memory, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.size, size);
	cr_assert_eq(info.memory_type, MEMORY_TYPE_NORMAL);

	const struct memory_slice_request slice_request = {
		.header = {.op = MEMORY_OP_SLICE},
		.offset = 1u,
		.size   = TEST_MAPPING_GRANULE,
	};
	struct memory_slice_response slice;
	result = kernel_capability_test_call(memory, &slice_request, sizeof(slice_request), &slice, sizeof(slice));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	result = kernel_capability_test_call(slice.memory_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.size, TEST_MAPPING_GRANULE);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_is_whole_memory_and_last_authority_unmaps) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              allocator;
	cap_id_t                              memory;
	cap_id_t                              space_cap;
	struct address_space_map_response     mapped;
	struct mapping_info                   info;
	syscall_result_t                      result;
	size_t                                mappings_before;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping");
	mappings_before = address_space_mapping_count(process_address_space(ctx.process));
	allocator       = root_allocator(&ctx);
	memory          = allocate_memory(allocator, 2u * TEST_MAPPING_GRANULE);
	space_cap       = kernel_address_space_grant(
        ctx.process, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_MAP | CAP_DELEGATE | CAP_WRITE | CAP_EXEC);
	cr_assert_neq(space_cap, CAP_ID_INVALID);
	const struct address_space_map_request map_request = {
		.header       = {.op = ADDRESS_SPACE_OP_MAP},
		.memory_cap   = memory,
		.access       = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
		.alignment    = TEST_MAPPING_GRANULE,
		.guard_before = TEST_MAPPING_GRANULE,
	};
	result = kernel_capability_test_call(space_cap, &map_request, sizeof(map_request), &mapped, sizeof(mapped));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_mapping_count(process_address_space(ctx.process)), mappings_before + 1u);
	cr_assert(cap_destroy_by_id(memory));
	cr_assert(cap_destroy_by_id(space_cap));
	const struct mapping_info_request info_request = {.header = {.op = MAPPING_OP_INFO}};
	result = kernel_capability_test_call(mapped.mapping_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.address, mapped.address);
	cr_assert_eq(info.size, 2u * TEST_MAPPING_GRANULE);
	cr_assert_eq(info.guard_before, TEST_MAPPING_GRANULE);

	const struct mapping_protect_request protect = {
		.header = {.op = MAPPING_OP_PROTECT},
		.access = MEMORY_ACCESS_READ,
	};
	result = kernel_capability_test_call(mapped.mapping_cap, &protect, sizeof(protect), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(cap_destroy_by_id(mapped.mapping_cap));
	cr_assert_eq(address_space_mapping_count(process_address_space(ctx.process)), mappings_before);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, unaligned_slice_is_content_valid_but_not_mappable) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              allocator;
	cap_id_t                              memory;
	cap_id_t                              space_cap;
	struct memory_slice_response          slice;
	struct address_space_map_response     mapped;
	syscall_result_t                      result;
	size_t                                mappings_before;

	kernel_capability_test_begin(&ctx, "kernel-cap/unaligned-slice");
	mappings_before                                 = address_space_mapping_count(process_address_space(ctx.process));
	allocator                                       = root_allocator(&ctx);
	memory                                          = allocate_memory(allocator, 2u * TEST_MAPPING_GRANULE + 1u);
	const struct memory_slice_request slice_request = {
		.header = {.op = MEMORY_OP_SLICE},
		.offset = 1u,
		.size   = TEST_MAPPING_GRANULE,
	};
	result = kernel_capability_test_call(memory, &slice_request, sizeof(slice_request), &slice, sizeof(slice));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	space_cap = kernel_address_space_grant(
		ctx.process, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_MAP | CAP_WRITE | CAP_EXEC);
	cr_assert_neq(space_cap, CAP_ID_INVALID);
	const struct address_space_map_request map_request = {
		.header     = {.op = ADDRESS_SPACE_OP_MAP},
		.memory_cap = slice.memory_cap,
		.access     = MEMORY_ACCESS_READ,
	};
	result = kernel_capability_test_call(space_cap, &map_request, sizeof(map_request), &mapped, sizeof(mapped));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(address_space_mapping_count(process_address_space(ctx.process)), mappings_before);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, derived_allocator_attenuates_output_policy) {
	struct kernel_capability_test_context   ctx;
	cap_id_t                                root;
	struct memory_allocator_derive_response derived;
	struct memory_allocator_info            info;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/allocator-derive");
	root                                                 = root_allocator(&ctx);
	const struct memory_allocator_derive_request request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_DERIVE},
		.allocator_rights = CAP_CALL | CAP_READ | CAP_ALLOCATE,
		.memory_rights    = CAP_READ | CAP_WRITE | CAP_MAP | CAP_DERIVE,
		.memory_type_mask = 1ull << MEMORY_TYPE_NORMAL,
	};
	result = kernel_capability_test_call(root, &request, sizeof(request), &derived, sizeof(derived));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	const struct memory_allocator_info_request info_request = {
		.header = {.op = MEMORY_ALLOCATOR_OP_INFO},
	};
	result =
		kernel_capability_test_call(derived.allocator_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.memory_rights, request.memory_rights);
	cr_assert_eq(info.memory_type_mask, request.memory_type_mask);
	cr_assert_eq(info.claim_policy, MEMORY_ALLOCATOR_CLAIMS_NONE);
	cap_id_t memory = allocate_memory(derived.allocator_cap, 123u);
	cr_assert(cap_destroy_by_id(derived.allocator_cap));
	const struct memory_info_request memory_info_request = {.header = {.op = MEMORY_OP_INFO}};
	struct memory_info               memory_info;
	result = kernel_capability_test_call(
		memory, &memory_info_request, sizeof(memory_info_request), &memory_info, sizeof(memory_info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(memory_info.size, 123u);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, allocator_without_normal_memory_denies_alloc) {
	struct kernel_capability_test_context   ctx;
	struct memory_allocator_derive_response derived;
	struct memory_allocator_alloc_response  allocation;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/allocator-no-normal");
	cap_id_t                                     root           = root_allocator(&ctx);
	const struct memory_allocator_derive_request derive_request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_DERIVE},
		.allocator_rights = CAP_CALL | CAP_ALLOCATE,
		.memory_rights    = CAP_READ | CAP_WRITE | CAP_MAP,
		.memory_type_mask = 0u,
	};
	result = kernel_capability_test_call(root, &derive_request, sizeof(derive_request), &derived, sizeof(derived));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	const struct memory_allocator_alloc_request alloc_request = {
		.header = {.op = MEMORY_ALLOCATOR_OP_ALLOC},
		.size   = TEST_MAPPING_GRANULE,
	};
	result = kernel_capability_test_call(
		derived.allocator_cap, &alloc_request, sizeof(alloc_request), &allocation, sizeof(allocation));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, restricted_allocator_enforces_and_inherits_claim_ranges) {
	struct kernel_capability_test_context           ctx;
	struct memory_allocator_derive_response         restricted;
	struct memory_allocator_derive_response         inherited;
	struct memory_allocator_claim_physical_response claimed;
	syscall_result_t                                result;
	const uintptr_t                                 base    = 0x100000000ull;
	const size_t                                    granule = TEST_MAPPING_GRANULE;
	uint64_t payload[(sizeof(struct memory_allocator_derive_request) + sizeof(struct memory_allocator_physical_range) +
	                  sizeof(uint64_t) - 1u) /
	                 sizeof(uint64_t)]                      = {0};
	struct memory_allocator_derive_request* request         = (struct memory_allocator_derive_request*)payload;

	kernel_capability_test_begin(&ctx, "kernel-cap/allocator-ranges");
	cap_id_t root              = root_allocator(&ctx);
	request->header.op         = MEMORY_ALLOCATOR_OP_DERIVE;
	request->allocator_rights  = CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DERIVE;
	request->memory_rights     = CAP_READ | CAP_MAP;
	request->memory_type_mask  = 1ull << MEMORY_TYPE_DEVICE;
	request->claim_range_count = 1u;
	request->claim_ranges[0]   = (struct memory_allocator_physical_range){base, 4u * granule};
	result                     = kernel_capability_test_call(
        root, request, sizeof(*request) + sizeof(request->claim_ranges[0]), &restricted, sizeof(restricted));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = claim_memory(restricted.allocator_cap, base + granule, granule, MEMORY_TYPE_DEVICE, &claimed);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(cap_destroy_by_id(claimed.memory_cap));
	result = claim_memory(restricted.allocator_cap, base + 4u * granule, granule, MEMORY_TYPE_DEVICE, &claimed);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	result = claim_memory(restricted.allocator_cap, base + 3u * granule, 2u * granule, MEMORY_TYPE_DEVICE, &claimed);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	const struct memory_allocator_derive_request inherit_request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_DERIVE},
		.allocator_rights = CAP_CALL | CAP_READ | CAP_MANAGE,
		.memory_rights    = CAP_READ | CAP_MAP,
		.memory_type_mask = 1ull << MEMORY_TYPE_DEVICE,
		.flags            = MEMORY_ALLOCATOR_DERIVE_INHERIT_CLAIMS,
	};
	result = kernel_capability_test_call(
		restricted.allocator_cap, &inherit_request, sizeof(inherit_request), &inherited, sizeof(inherited));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	result = claim_memory(inherited.allocator_cap, base + 2u * granule, granule, MEMORY_TYPE_DEVICE, &claimed);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(cap_destroy_by_id(claimed.memory_cap));
	result = claim_memory(inherited.allocator_cap, base + 4u * granule, granule, MEMORY_TYPE_DEVICE, &claimed);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_has_one_resource_and_peer_authority_survives_initial_drop) {
	struct kernel_capability_test_context ctx;
	struct memory*                        memory;
	struct mapping*                       mapping;
	struct capability*                    initial;
	struct capability*                    peer;
	struct mapping_info                   info;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-topology");
	cr_assert(memory_create_anonymous(TEST_MAPPING_GRANULE, &memory));
	cr_assert(address_space_map(process_address_space(ctx.process),
	                            &(const struct address_space_mapping_request){
									.memory = memory,
									.access = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
								},
	                            &mapping));
	memory_release(memory);
	cap_id_t initial_id = kernel_mapping_publish(ctx.process,
	                                             process_pid(ctx.process),
	                                             mapping,
	                                             CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DESTROY |
	                                                 CAP_DELEGATE | CAP_DELEGATE_PEER,
	                                             MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE);
	cr_assert_neq(initial_id, CAP_ID_INVALID);
	cr_assert_eq(
		kernel_mapping_publish(ctx.process, process_pid(ctx.process), mapping, CAP_CALL | CAP_READ, MEMORY_ACCESS_READ),
		CAP_ID_INVALID);
	initial = cap_acquire(initial_id);
	cr_assert_not_null(initial);
	cap_id_t peer_id = cap_delegate_create(
		initial, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_MAP | CAP_DESTROY | CAP_DELEGATE, true);
	cap_release(initial);
	cr_assert_neq(peer_id, CAP_ID_INVALID);
	peer = cap_acquire(peer_id);
	cr_assert_not_null(peer);
	cap_id_t descendant_id =
		cap_delegate_create(peer, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_DESTROY, false);
	cap_release(peer);
	cr_assert_neq(descendant_id, CAP_ID_INVALID);
	cr_assert(cap_destroy_by_id(initial_id));
	const struct mapping_info_request info_request = {.header = {.op = MAPPING_OP_INFO}};
	result = kernel_capability_test_call(peer_id, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	const struct mapping_unmap_request unmap_request = {.header = {.op = MAPPING_OP_UNMAP}};
	result = kernel_capability_test_call(peer_id, &unmap_request, sizeof(unmap_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_acquire(peer_id));
	cr_assert_null(cap_acquire(descendant_id));
	mapping_release(mapping);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, revoking_parent_memory_revokes_slices) {
	struct kernel_capability_test_context ctx;
	struct memory_slice_response          slice;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-slice-revocation");
	cap_id_t                          allocator = root_allocator(&ctx);
	cap_id_t                          memory    = allocate_memory(allocator, 2u * TEST_MAPPING_GRANULE);
	const struct memory_slice_request request   = {
		  .header = {.op = MEMORY_OP_SLICE},
		  .offset = TEST_MAPPING_GRANULE,
		  .size   = TEST_MAPPING_GRANULE,
    };
	result = kernel_capability_test_call(memory, &request, sizeof(request), &slice, sizeof(slice));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	struct capability* slice_cap = cap_acquire(slice.memory_cap);
	cr_assert_not_null(slice_cap);
	cap_release(slice_cap);
	cr_assert(cap_destroy_by_id(memory));
	cr_assert_null(cap_acquire(slice.memory_cap));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, physical_zero_is_valid_exclusive_and_device_is_never_executable) {
	struct kernel_capability_test_context           ctx;
	struct memory_allocator_claim_physical_response first;
	struct memory_allocator_claim_physical_response second;
	struct capability*                              capability;
	cap_id_t                                        allocator;
	syscall_result_t                                result;

	kernel_capability_test_begin(&ctx, "kernel-cap/physical-claim");
	allocator = root_allocator(&ctx);
	result    = claim_memory(allocator, 0u, TEST_MAPPING_GRANULE, MEMORY_TYPE_DEVICE, &first);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	capability = cap_acquire(first.memory_cap);
	cr_assert_not_null(capability);
	cr_assert_eq(capability->rights & CAP_EXEC, 0u);
	cap_release(capability);
	result = claim_memory(allocator, 0u, TEST_MAPPING_GRANULE, MEMORY_TYPE_DEVICE, &second);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert(cap_destroy_by_id(first.memory_cap));
	result = claim_memory(allocator, 0u, TEST_MAPPING_GRANULE, MEMORY_TYPE_DEVICE, &second);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	kernel_capability_test_end(&ctx);
}
