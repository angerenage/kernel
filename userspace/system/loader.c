#include <base/loader.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/loader.h>

syscall_status_t loader_load(cap_id_t loader_cap, cap_id_t module_cap, struct loader_load_response* out_response) {
	const struct loader_load_request request = {.module_cap = module_cap};
	syscall_result_t                 result;

	if (loader_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(loader_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (module_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(module_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_response == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_response);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(loader_cap, &request, sizeof(request), out_response, sizeof(*out_response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(loader_load, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("loader_load returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (out_response->process_cap == CAP_ID_INVALID || out_response->address_space_cap == CAP_ID_INVALID ||
	    out_response->entry == 0u || out_response->heap_base == 0u || out_response->heap_page_count == 0u) {
		RUNTIME_DIAGNOSTIC_FAILED(loader_load);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}
