#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/thread.h>

#include "syscall.h"

bool thread_join(cap_id_t cap, uintptr_t* out_exit_code) {
	struct thread_join_request  request = {.header = {.op = THREAD_OP_JOIN}};
	struct thread_join_response response;
	syscall_result_t            result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));

	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	if (out_exit_code != NULL) *out_exit_code = response.exit_code;
	return true;
}

bool thread_detach(cap_id_t cap) {
	struct thread_detach_request request = {.header = {.op = THREAD_OP_DETACH}};
	syscall_result_t             result  = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool thread_cancel(cap_id_t cap) {
	struct thread_cancel_request request = {.header = {.op = THREAD_OP_CANCEL}};
	syscall_result_t             result  = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool thread_set_cancel_enabled(cap_id_t cap, bool enabled) {
	struct thread_set_cancel_enabled_request request = {
		.header  = {.op = THREAD_OP_SET_CANCEL_ENABLED},
		.enabled = enabled,
	};
	syscall_result_t result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool thread_test_cancel(cap_id_t cap, bool* out_should_cancel) {
	struct thread_test_cancel_request  request = {.header = {.op = THREAD_OP_TEST_CANCEL}};
	struct thread_test_cancel_response response;
	syscall_result_t result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));

	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	if (out_should_cancel != NULL) *out_should_cancel = response.should_cancel;
	return true;
}
