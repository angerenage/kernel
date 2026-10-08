#include <base/math.h>
#include <libc/stdlib.h>
#include <runtime/diagnostic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/module.h>

#include "syscall.h"

static bool module_provider_entry_valid(const struct module_provider_entry* entry) {
	return entry->id != MODULE_ID_INVALID && entry->reserved == 0u &&
	       memchr(entry->name, '\0', sizeof(entry->name)) != NULL &&
	       memchr(entry->path, '\0', sizeof(entry->path)) != NULL;
}

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

syscall_status_t module_enumerate(cap_id_t modules_provider_cap, uint64_t offset, struct module_provider_entry* entries,
                                  size_t count, size_t* out_returned, uint64_t* out_total) {
	const size_t maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(struct module_provider_enumerate_response)) / sizeof(*entries);
	struct module_provider_enumerate_request request = {
		.header   = {.op = MODULE_PROVIDER_OP_ENUMERATE},
		.reserved = 0u,
		.offset   = offset,
	};
	struct module_provider_enumerate_response* response;
	syscall_result_t                           result;
	size_t                                     capacity;
	size_t                                     transfer_count;

	if (out_returned == NULL || out_total == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_returned = 0u;
	*out_total    = 0u;
	if (modules_provider_cap == CAP_ID_INVALID || (count != 0u && entries == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;
	transfer_count = count > maximum_count ? maximum_count : count;
	request.count  = transfer_count;
	capacity       = sizeof(*response) + transfer_count * sizeof(*entries);
	response       = malloc(capacity);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	result = cap_call_syscall(modules_provider_cap, &request, sizeof(request), response, capacity);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(MODULE_PROVIDER_OP_ENUMERATE, result);
	if (result.status != SYSCALL_STATUS_OK) {
		free(response);
		return result.status;
	}
	if (result.value < sizeof(*response) || response->returned > transfer_count ||
	    response->returned > response->total ||
	    (response->returned != 0u && (offset > response->total || response->returned > response->total - offset)) ||
	    result.value != sizeof(*response) + (size_t)response->returned * sizeof(*entries)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		if (!module_provider_entry_valid(&response->entries[i])) {
			free(response);
			return SYSCALL_STATUS_FAILED;
		}
	}
	if (response->returned != 0u) memcpy(entries, response->entries, (size_t)response->returned * sizeof(*entries));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
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
	if (response.mapping_cap == CAP_ID_INVALID || response.address == 0u || response.mapping_size == 0u ||
	    response.data_offset >= response.mapping_size) {
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
