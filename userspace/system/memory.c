#include <base/address_space.h>
#include <base/memory.h>
#include <runtime/diagnostic.h>
#include <system/capability.h>
#include <system/memory.h>

static syscall_status_t fixed_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                   size_t response_size, uint64_t operation) {
	(void)operation;
	if (cap == CAP_ID_INVALID || request == NULL || (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(cap, request, request_size, response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t memory_allocator_info(cap_id_t allocator_cap, struct memory_allocator_info* out_info) {
	const struct memory_allocator_info_request request = {.header = {.op = MEMORY_ALLOCATOR_OP_INFO}};
	return fixed_call(allocator_cap, &request, sizeof(request), out_info, sizeof(*out_info), MEMORY_ALLOCATOR_OP_INFO);
}

syscall_status_t memory_allocator_derive(cap_id_t allocator_cap, const struct memory_allocator_derive_request* request,
                                         size_t request_size, cap_id_t* out_allocator_cap) {
	struct memory_allocator_derive_response response;
	if (request == NULL || request_size < sizeof(*request) || out_allocator_cap == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_allocator_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(allocator_cap, request, request_size, &response, sizeof(response), MEMORY_ALLOCATOR_OP_DERIVE);
	if (status == SYSCALL_STATUS_OK) *out_allocator_cap = response.allocator_cap;
	return status;
}

syscall_status_t memory_allocator_alloc(cap_id_t allocator_cap, size_t size, cap_id_t* out_memory_cap) {
	const struct memory_allocator_alloc_request request = {
		.header = {.op = MEMORY_ALLOCATOR_OP_ALLOC},
		.size   = size,
	};
	struct memory_allocator_alloc_response response;
	if (out_memory_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_memory_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(allocator_cap, &request, sizeof(request), &response, sizeof(response), MEMORY_ALLOCATOR_OP_ALLOC);
	if (status == SYSCALL_STATUS_OK) *out_memory_cap = response.memory_cap;
	return status;
}

syscall_status_t memory_allocator_claim_physical(cap_id_t allocator_cap, uintptr_t physical_address, size_t size,
                                                 enum memory_type memory_type, cap_id_t* out_memory_cap) {
	const struct memory_allocator_claim_physical_request request = {
		.header           = {.op = MEMORY_ALLOCATOR_OP_CLAIM_PHYSICAL},
		.physical_address = physical_address,
		.size             = size,
		.memory_type      = memory_type,
	};
	struct memory_allocator_claim_physical_response response;
	if (out_memory_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_memory_cap         = CAP_ID_INVALID;
	syscall_status_t status = fixed_call(
		allocator_cap, &request, sizeof(request), &response, sizeof(response), MEMORY_ALLOCATOR_OP_CLAIM_PHYSICAL);
	if (status == SYSCALL_STATUS_OK) *out_memory_cap = response.memory_cap;
	return status;
}

syscall_status_t memory_info(cap_id_t memory_cap, struct memory_info* out_info) {
	const struct memory_info_request request = {.header = {.op = MEMORY_OP_INFO}};
	return fixed_call(memory_cap, &request, sizeof(request), out_info, sizeof(*out_info), MEMORY_OP_INFO);
}

syscall_status_t memory_slice(cap_id_t memory_cap, size_t offset, size_t size, cap_id_t* out_memory_cap) {
	const struct memory_slice_request request = {
		.header = {.op = MEMORY_OP_SLICE},
		.offset = offset,
		.size   = size,
	};
	struct memory_slice_response response;
	if (out_memory_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_memory_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(memory_cap, &request, sizeof(request), &response, sizeof(response), MEMORY_OP_SLICE);
	if (status == SYSCALL_STATUS_OK) *out_memory_cap = response.memory_cap;
	return status;
}

syscall_status_t address_space_info(cap_id_t address_space_cap, struct address_space_info* out_info) {
	const struct address_space_info_request request = {.header = {.op = ADDRESS_SPACE_OP_INFO}};
	return fixed_call(address_space_cap, &request, sizeof(request), out_info, sizeof(*out_info), ADDRESS_SPACE_OP_INFO);
}

syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t memory_cap, memory_access_t access,
                                   uintptr_t address, size_t alignment, size_t guard_before, size_t guard_after,
                                   struct address_space_map_response* out_response) {
	const struct address_space_map_request request = {
		.header       = {.op = ADDRESS_SPACE_OP_MAP},
		.memory_cap   = memory_cap,
		.access       = access,
		.address      = address,
		.alignment    = alignment,
		.guard_before = guard_before,
		.guard_after  = guard_after,
	};
	return fixed_call(
		address_space_cap, &request, sizeof(request), out_response, sizeof(*out_response), ADDRESS_SPACE_OP_MAP);
}

syscall_status_t mapping_info(cap_id_t mapping_cap, struct mapping_info* out_info) {
	const struct mapping_info_request request = {.header = {.op = MAPPING_OP_INFO}};
	return fixed_call(mapping_cap, &request, sizeof(request), out_info, sizeof(*out_info), MAPPING_OP_INFO);
}

syscall_status_t mapping_protect(cap_id_t mapping_cap, memory_access_t access) {
	const struct mapping_protect_request request = {.header = {.op = MAPPING_OP_PROTECT}, .access = access};
	return fixed_call(mapping_cap, &request, sizeof(request), NULL, 0u, MAPPING_OP_PROTECT);
}

syscall_status_t mapping_unmap(cap_id_t mapping_cap) {
	const struct mapping_unmap_request request = {.header = {.op = MAPPING_OP_UNMAP}};
	return fixed_call(mapping_cap, &request, sizeof(request), NULL, 0u, MAPPING_OP_UNMAP);
}

syscall_status_t mapping_sync(cap_id_t mapping_cap, size_t offset, size_t size, enum dma_sync_target target) {
	const struct mapping_sync_request request = {
		.header   = {.op = MAPPING_OP_SYNC},
		.target   = target,
		.reserved = 0u,
		.offset   = offset,
		.size     = size,
	};
	return fixed_call(mapping_cap, &request, sizeof(request), NULL, 0u, MAPPING_OP_SYNC);
}
