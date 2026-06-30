#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/process.h>

#include "syscall.h"

bool process_create(const char* name, size_t name_length, struct process_create_response* out_response) {
	syscall_result_t result;

	if (out_response == NULL || name == NULL) return false;
	result =
		syscall(SYSCALL_CREATE_PROCESS, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)out_response, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool process_self_info(struct self_info* out_info) {
	syscall_result_t result;

	if (out_info == NULL) return false;
	result = syscall(SYSCALL_SELF, (uintptr_t)out_info, 0u, 0u, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool process_get_info(cap_id_t cap, struct process_info_response* out_info) {
	struct process_info_request request = {.header = {.op = PROCESS_OP_INFO}};
	syscall_result_t            result;

	if (out_info == NULL) return false;
	result = cap_call_syscall(cap, &request, sizeof(request), out_info, sizeof(*out_info));
	if (result.status != SYSCALL_STATUS_OK) return false;
	return result.value == sizeof(*out_info);
}

bool process_run(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages, cap_id_t* out_thread_cap) {
	struct process_info_response proc;
	struct process_run_request   request;
	struct process_run_response  response;
	syscall_result_t             result;
	cap_id_t                     serial_cap = CAP_ID_INVALID;
	bool                         have_pid;

	(void)arg;

	if (out_thread_cap == NULL || cap == CAP_ID_INVALID) return false;

	have_pid = process_get_info(cap, &proc) && proc.pid != PROCESS_PID_INVALID;
	if (have_pid && serial_cap_id != CAP_ID_INVALID) {
		(void)cap_delegate(serial_cap_id, proc.pid, CAP_READ | CAP_DELEGATE, &serial_cap);
	}

	request = (struct process_run_request){
		.header      = {.op = PROCESS_OP_RUN},
		.entry       = entry,
		.arg         = (uintptr_t)&serial_cap,
		.stack_pages = stack_pages,
	};
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_thread_cap = response.thread_cap;
	return response.thread_cap != CAP_ID_INVALID;
}

bool process_spawn_thread(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages, const char* name,
                          size_t name_length, cap_id_t* out_thread_cap) {
	struct process_spawn_thread_request request = {
		.header      = {.op = PROCESS_OP_SPAWN_THREAD},
		.entry       = entry,
		.arg         = arg,
		.stack_pages = stack_pages,
		.name        = name,
		.name_length = name_length,
	};
	struct process_spawn_thread_response response;
	syscall_result_t                     result;

	if (out_thread_cap == NULL) return false;
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_thread_cap = response.thread_cap;
	return response.thread_cap != CAP_ID_INVALID;
}

bool process_wait(cap_id_t cap, uintptr_t* out_exit_code) {
	struct process_wait_request  request = {.header = {.op = PROCESS_OP_WAIT}};
	struct process_wait_response response;
	syscall_result_t             result;

	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) return false;
	if (result.value != sizeof(response)) return false;
	if (out_exit_code != NULL) *out_exit_code = response.exit_code;
	return true;
}

bool process_detach(cap_id_t cap) {
	struct process_detach_request request = {.header = {.op = PROCESS_OP_DETACH}};
	syscall_result_t              result;

	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool process_kill(cap_id_t cap, uintptr_t exit_code) {
	struct process_kill_request request = {.header = {.op = PROCESS_OP_KILL}, .exit_code = exit_code};
	syscall_result_t            result;

	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

__attribute__((noreturn))
void process_exit(uintptr_t code) {
	for (;;) {
		(void)syscall(SYSCALL_EXIT_PROCESS, code, 0u, 0u, 0u, 0u, 0u);
		(void)syscall(SYSCALL_EXIT_THREAD, code, 0u, 0u, 0u, 0u, 0u);
	}
}
