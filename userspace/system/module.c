#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/module.h>

#include "syscall.h"

syscall_status_t module_resolve(const char* name, size_t name_length, process_id_t target, cap_id_t* out_module_cap) {
	syscall_result_t result;

	if (out_module_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (name == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = syscall(SYSCALL_MODULE_RESOLVE, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)target, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name_length);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(target);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_MODULE_RESOLVE, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_MODULE_RESOLVE, result);
	}
#endif

	if (result.status != SYSCALL_STATUS_OK) return result.status;
	*out_module_cap = (cap_id_t)result.value;
	return SYSCALL_STATUS_OK;
}

syscall_status_t module_map(cap_id_t module_cap, uintptr_t* out_mapped_base) {
	struct {
		uintptr_t base_out_ptr;
	} request;

	struct {
		uintptr_t mapped_base;
	} response;

	syscall_result_t result;

	if (module_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	request.base_out_ptr = (uintptr_t)out_mapped_base;
	result               = cap_call_syscall(module_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(module_map, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("module_map returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (out_mapped_base != NULL) *out_mapped_base = response.mapped_base;
	return SYSCALL_STATUS_OK;
}
