#include <runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <syscall.h>

bool process_get_info(cap_id_t cap, struct process_info_response* out_info) {
	struct process_info_request request = {.header = {.op = PROCESS_OP_INFO}};
	syscall_result_t            result;

	if (out_info == NULL) return false;

	result = cap_call(cap, &request, sizeof(request));
	if (result.status != SYSCALL_STATUS_OK) return false;

	if (result.value != 0) return false;

	return recv_message(out_info, sizeof(*out_info), NULL, NULL).status == SYSCALL_STATUS_OK;
}

bool process_run(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages) {
	struct process_run_request request = {
		.header = {.op = PROCESS_OP_RUN}, .entry = entry, .arg = arg, .stack_pages = stack_pages};
	syscall_result_t result;

	result = cap_call(cap, &request, sizeof(request));
	return result.status == SYSCALL_STATUS_OK;
}

bool process_wait(cap_id_t cap, uintptr_t* out_exit_code) {
	struct process_wait_request  request = {.header = {.op = PROCESS_OP_WAIT}};
	struct process_wait_response response;
	syscall_result_t             result;

	result = cap_call(cap, &request, sizeof(request));
	if (result.status != SYSCALL_STATUS_OK) return false;

	if (result.value != 0) return false;

	if (recv_message(&response, sizeof(response), NULL, NULL).status != SYSCALL_STATUS_OK) return false;

	if (out_exit_code != NULL) *out_exit_code = response.exit_code;

	return true;
}

bool process_detach(cap_id_t cap) {
	struct process_detach_request request = {.header = {.op = PROCESS_OP_DETACH}};
	syscall_result_t              result;

	result = cap_call(cap, &request, sizeof(request));
	return result.status == SYSCALL_STATUS_OK;
}

bool process_kill(cap_id_t cap, uintptr_t exit_code) {
	struct process_kill_request request = {.header = {.op = PROCESS_OP_KILL}, .exit_code = exit_code};
	syscall_result_t            result;

	result = cap_call(cap, &request, sizeof(request));
	return result.status == SYSCALL_STATUS_OK;
}
