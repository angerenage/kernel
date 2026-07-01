#include <runtime/diagnostic.h>
#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/thread.h>

#include "syscall.h"

syscall_status_t thread_join(cap_id_t cap, uintptr_t* out_exit_code) {
	struct thread_join_request  request = {.header = {.op = THREAD_OP_JOIN}};
	struct thread_join_response response;
	syscall_result_t            result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(THREAD_OP_JOIN, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("THREAD_OP_JOIN returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (out_exit_code != NULL) *out_exit_code = response.exit_code;
	return SYSCALL_STATUS_OK;
}

syscall_status_t thread_detach(cap_id_t cap) {
	struct thread_detach_request request = {.header = {.op = THREAD_OP_DETACH}};
	syscall_result_t             result;
	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(THREAD_OP_DETACH, result);
	return result.status;
}

syscall_status_t thread_cancel(cap_id_t cap) {
	struct thread_cancel_request request = {.header = {.op = THREAD_OP_CANCEL}};
	syscall_result_t             result;
	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(THREAD_OP_CANCEL, result);
	return result.status;
}

syscall_status_t thread_set_cancel_enabled(cap_id_t cap, bool enabled) {
	struct thread_set_cancel_enabled_request request = {
		.header  = {.op = THREAD_OP_SET_CANCEL_ENABLED},
		.enabled = enabled,
	};
	syscall_result_t result;
	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(THREAD_OP_SET_CANCEL_ENABLED, result);
	return result.status;
}

syscall_status_t thread_test_cancel(cap_id_t cap, bool* out_should_cancel) {
	struct thread_test_cancel_request  request = {.header = {.op = THREAD_OP_TEST_CANCEL}};
	struct thread_test_cancel_response response;
	syscall_result_t                   result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(THREAD_OP_TEST_CANCEL, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"THREAD_OP_TEST_CANCEL returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (out_should_cancel != NULL) *out_should_cancel = response.should_cancel;
	return SYSCALL_STATUS_OK;
}
