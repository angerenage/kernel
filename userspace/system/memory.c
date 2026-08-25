#include <runtime/diagnostic.h>
#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/memory.h>

#include "syscall.h"

syscall_status_t memory_create(const struct memory_create_params* params, cap_id_t* out_memory_cap) {
	if (params == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(params);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_memory_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_memory_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	syscall_result_t result = syscall(SYSCALL_MEMORY_CREATE, (uintptr_t)params, sizeof(*params), 0u, 0u, 0u, 0u);
#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(params);
	else RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_MEMORY_CREATE, result);
#endif
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	*out_memory_cap = (cap_id_t)result.value;
	return SYSCALL_STATUS_OK;
}

syscall_status_t memory_get_info(cap_id_t memory_cap, struct memory_info* out_info) {
	const struct memory_info_request request = {.header = {.op = MEMORY_OP_INFO}};
	if (memory_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(memory_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MEMORY_OP_INFO, result);
	return result.status == SYSCALL_STATUS_OK && result.value != sizeof(*out_info) ? SYSCALL_STATUS_FAILED
	                                                                               : result.status;
}

static syscall_status_t memory_transfer(cap_id_t cap, size_t offset, uintptr_t buffer, size_t size, bool reading) {
	union {
		struct memory_read_request  read;
		struct memory_write_request write;
	} request;
	if (cap == CAP_ID_INVALID || (size != 0u && buffer == 0u)) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (reading) {
		request.read = (struct memory_read_request){
			.header = {.op = MEMORY_OP_READ}, .offset = offset, .destination = buffer, .size = size};
	}
	else {
		request.write = (struct memory_write_request){
			.header = {.op = MEMORY_OP_WRITE}, .source = buffer, .offset = offset, .size = size};
	}
	struct memory_transfer_response response;
	syscall_result_t                result = cap_call_syscall(
        cap, &request, reading ? sizeof(request.read) : sizeof(request.write), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(reading ? MEMORY_OP_READ : MEMORY_OP_WRITE, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == sizeof(response) && response.bytes_transferred == size ? SYSCALL_STATUS_OK
	                                                                              : SYSCALL_STATUS_FAILED;
}

syscall_status_t memory_read(cap_id_t memory_cap, size_t offset, void* destination, size_t size) {
	return memory_transfer(memory_cap, offset, (uintptr_t)destination, size, true);
}

syscall_status_t memory_write(cap_id_t memory_cap, size_t offset, const void* source, size_t size) {
	return memory_transfer(memory_cap, offset, (uintptr_t)source, size, false);
}

syscall_status_t address_space_map(cap_id_t address_space_cap, cap_id_t memory_cap,
                                   const struct memory_map_params*  params,
                                   struct address_space_map_result* out_result) {
	if (address_space_cap == CAP_ID_INVALID || memory_cap == CAP_ID_INVALID || params == NULL || out_result == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	const struct address_space_map_request request = {
		.header = {.op = ADDRESS_SPACE_OP_MAP}, .memory_cap = memory_cap, .params = *params};
	syscall_result_t result =
		cap_call_syscall(address_space_cap, &request, sizeof(request), out_result, sizeof(*out_result));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ADDRESS_SPACE_OP_MAP, result);
	return result.status == SYSCALL_STATUS_OK && result.value != sizeof(*out_result) ? SYSCALL_STATUS_FAILED
	                                                                                 : result.status;
}

syscall_status_t mapping_get_info(cap_id_t mapping_cap, struct vmm_info* out_info) {
	const struct mapping_info_request request = {.header = {.op = MAPPING_OP_INFO}};
	struct mapping_info_response      response;
	if (mapping_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(mapping_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) return SYSCALL_STATUS_FAILED;
	*out_info = response.info;
	return SYSCALL_STATUS_OK;
}

syscall_status_t mapping_protect(cap_id_t mapping_cap, vmm_prot_t prot) {
	const struct mapping_protect_request request = {.header = {.op = MAPPING_OP_PROTECT}, .prot = prot};
	if (mapping_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_PROTECT, result);
	return result.status;
}

syscall_status_t mapping_unmap(cap_id_t mapping_cap) {
	const struct mapping_unmap_request request = {.header = {.op = MAPPING_OP_UNMAP}};
	if (mapping_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MAPPING_OP_UNMAP, result);
	return result.status;
}
