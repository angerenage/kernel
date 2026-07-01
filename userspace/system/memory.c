#include <runtime/diagnostic.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/memory.h>

#include "syscall.h"

syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t allocation_cap, cap_id_t* out_mapping_cap) {
	const struct address_space_map_request request = {.header         = {.op = ADDRESS_SPACE_OP_MAP},
	                                                  .allocation_cap = allocation_cap};
	struct address_space_map_response      response;
	syscall_result_t                       result;

	if (address_space_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(address_space_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (allocation_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_mapping_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_mapping_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ADDRESS_SPACE_OP_MAP, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ADDRESS_SPACE_OP_MAP returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_mapping_cap = response.mapping_cap;
	return SYSCALL_STATUS_OK;
}

syscall_status_t address_space_map_at(cap_id_t address_space_cap, cap_id_t allocation_cap, uintptr_t address,
                                      cap_id_t* out_mapping_cap) {
	const struct address_space_map_at_request request = {
		.header = {.op = ADDRESS_SPACE_OP_MAP_AT}, .allocation_cap = allocation_cap, .address = address};
	struct address_space_map_response response;
	syscall_result_t                  result;

	if (address_space_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(address_space_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (allocation_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_mapping_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_mapping_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ADDRESS_SPACE_OP_MAP_AT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ADDRESS_SPACE_OP_MAP_AT returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_mapping_cap = response.mapping_cap;
	return SYSCALL_STATUS_OK;
}

syscall_status_t allocation_write(cap_id_t allocation_cap, uintptr_t dst_offset, const void* src, size_t size) {
	const struct allocation_copy_to_request request = {
		.header      = {.op = ALLOCATION_OP_COPY_TO},
		.src_address = (uintptr_t)src,
		.dst_offset  = dst_offset,
		.size        = size,
	};
	struct allocation_copy_response response;
	syscall_result_t                result;

	if (allocation_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size != 0u && src == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(src);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(allocation_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ALLOCATION_OP_COPY_TO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ALLOCATION_OP_COPY_TO returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (response.bytes_copied != size) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ALLOCATION_OP_COPY_TO copied an unexpected byte count", "bytes_copied", response.bytes_copied);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t allocation_read(cap_id_t allocation_cap, uintptr_t src_offset, void* dst, size_t size) {
	const struct allocation_copy_from_request request = {
		.header      = {.op = ALLOCATION_OP_COPY_FROM},
		.src_offset  = src_offset,
		.dst_address = (uintptr_t)dst,
		.size        = size,
	};
	struct allocation_copy_response response;
	syscall_result_t                result;

	if (allocation_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size != 0u && dst == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(dst);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(allocation_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ALLOCATION_OP_COPY_FROM, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ALLOCATION_OP_COPY_FROM returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (response.bytes_copied != size) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ALLOCATION_OP_COPY_FROM copied an unexpected byte count", "bytes_copied", response.bytes_copied);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t mapping_get_info(cap_id_t mapping_cap, struct vmm_info* out_info) {
	const struct mapping_read_request request = {.header = {.op = MAPPING_OP_READ}};
	struct mapping_read_response      response;
	syscall_result_t                  result;

	if (mapping_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(mapping_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(mapping_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("MAPPING_OP_READ returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_info = response.info;
	return SYSCALL_STATUS_OK;
}

syscall_status_t mapping_unmap(cap_id_t mapping_cap) {
	const struct mapping_unmap_request request = {.header = {.op = MAPPING_OP_UNMAP}};
	syscall_result_t                   result;

	if (mapping_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(mapping_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_UNMAP, result);
	return result.status;
}

syscall_status_t mapping_protect(cap_id_t mapping_cap, vmm_prot_t prot) {
	const struct mapping_protect_request request = {.header = {.op = MAPPING_OP_PROTECT}, .prot = prot};
	syscall_result_t                     result;
	if (mapping_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(mapping_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_PROTECT, result);
	return result.status;
}

syscall_status_t allocation_free(cap_id_t allocation_cap) {
	const struct allocation_free_request request = {.header = {.op = ALLOCATION_OP_FREE}};
	syscall_result_t                     result;
	if (allocation_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(allocation_cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ALLOCATION_OP_FREE, result);
	return result.status;
}

syscall_status_t address_space_query(cap_id_t address_space_cap, vmm_id_t id, struct vmm_info* out_info) {
	const struct address_space_query_request request = {.header = {.op = ADDRESS_SPACE_OP_QUERY}, .id = id};
	struct address_space_query_response      response;
	syscall_result_t                         result;

	if (address_space_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(address_space_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ADDRESS_SPACE_OP_QUERY, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"ADDRESS_SPACE_OP_QUERY returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_info = response.info;
	return SYSCALL_STATUS_OK;
}

syscall_status_t memory_allocate(size_t page_count, vmm_prot_t prot, enum vmm_kind kind, cap_id_t* out_allocation_cap) {
	syscall_result_t result;

	if (out_allocation_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_allocation_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_MEMORY_ALLOCATE, (uintptr_t)page_count, (uintptr_t)prot, (uintptr_t)kind, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(page_count);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(prot);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(kind);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_MEMORY_ALLOCATE, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_MEMORY_ALLOCATE, result);
	}
#endif

	if (result.status != SYSCALL_STATUS_OK) return result.status;
	*out_allocation_cap = (cap_id_t)result.value;
	return SYSCALL_STATUS_OK;
}
