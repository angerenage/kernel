#include <base/math.h>
#include <base/vmm.h>
#include <libc/stdlib.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/module.h>

#include "syscall.h"

syscall_status_t module_resolve(cap_id_t modules_provider_cap, const char* name, size_t name_length,
                                struct module_provider_resolve_response* out_module) {
	struct module_provider_resolve_request* request;
	syscall_result_t                        result;
	size_t                                  request_size;

	if (modules_provider_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(modules_provider_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (name == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (name_length == 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name_length);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_module == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_module);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (add_overflow_size(sizeof(*request), name_length, &request_size) || request_size > CAP_MAX_REQUEST_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name_length);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	request = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	*request = (struct module_provider_resolve_request){
		.header    = {.op = MODULE_PROVIDER_OP_RESOLVE},
		.name_size = name_length,
	};
	memcpy(request + 1, name, name_length);
	result = cap_call_syscall(modules_provider_cap, request, request_size, out_module, sizeof(*out_module));
	free(request);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MODULE_PROVIDER_OP_RESOLVE, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_module) || out_module->id == MODULE_ID_INVALID ||
	    out_module->cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("MODULE_PROVIDER_OP_RESOLVE returned an invalid response");
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t module_get_info(cap_id_t module_cap, struct module_info_response* out_info) {
	const struct module_info_request request = {.header = {.op = MODULE_OP_INFO}};
	syscall_result_t                 result;

	if (module_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(module_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MODULE_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_info)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("MODULE_OP_INFO returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t module_map(cap_id_t module_cap, struct module_map_response* out_mapping) {
	const struct module_map_request request = {.header = {.op = MODULE_OP_MAP}};
	struct module_map_response      response;
	syscall_result_t                result;

	if (module_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_mapping == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_mapping);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(module_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MODULE_OP_MAP, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("MODULE_OP_MAP returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (response.mapping_cap == CAP_ID_INVALID || response.mapping.id != VMM_ID_INVALID ||
	    response.mapping.base == NULL || response.mapping.page_count == 0u || response.mapping.prot != VMM_PROT_READ ||
	    response.mapping.guard_pages != 0u || response.data_offset >= VMM_PAGE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("MODULE_OP_MAP returned an invalid mapping");
		return SYSCALL_STATUS_FAILED;
	}
	*out_mapping = response;
	return SYSCALL_STATUS_OK;
}

syscall_status_t module_read(cap_id_t module_cap, uint64_t offset, void* buffer, size_t size) {
	const struct module_read_request request = {
		.header = {.op = MODULE_OP_READ},
		.offset = offset,
		.size   = size,
	};
	syscall_result_t result;

	if (module_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size != 0u && buffer == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(buffer);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size > CAP_MAX_RESPONSE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(module_cap, &request, sizeof(request), buffer, size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MODULE_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != size) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("MODULE_OP_READ returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}
