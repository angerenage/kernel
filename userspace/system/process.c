#include <base/cap.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/process.h>

#include "syscall.h"

syscall_status_t process_create(const char* name, size_t name_length, struct process_create_response* out_response) {
	syscall_result_t result;

	if (out_response == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_response);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (name == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result =
		syscall(SYSCALL_CREATE_PROCESS, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)out_response, 0u, 0u, 0u);

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
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_response);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CREATE_PROCESS, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CREATE_PROCESS, result);
	}
#endif

	return result.status;
}

syscall_status_t process_self_info(struct self_info* out_info) {
	syscall_result_t result;

	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_SELF, (uintptr_t)out_info, 0u, 0u, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SELF, result);
	}
#endif

	return result.status;
}

syscall_status_t process_get_info(cap_id_t cap, struct process_info_response* out_info) {
	struct process_info_request request = {.header = {.op = PROCESS_OP_INFO}};
	syscall_result_t            result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_info)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("PROCESS_OP_INFO returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t process_run(cap_id_t cap, uintptr_t entry, const void* arg_data, size_t arg_size,
                             cap_id_t* out_thread_cap) {
	struct process_run_request  request;
	struct process_run_response response;
	syscall_result_t            result;

	if (out_thread_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_thread_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (entry == 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(entry);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((arg_data == NULL) != (arg_size == 0u)) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(arg_data);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (arg_size > THREAD_START_ARG_MAX_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(arg_size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	request = (struct process_run_request){
		.header   = {.op = PROCESS_OP_RUN},
		.entry    = entry,
		.arg_data = arg_data,
		.arg_size = arg_size,
	};
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_RUN, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("PROCESS_OP_RUN returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_thread_cap = response.thread_cap;
	if (response.thread_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_FAILED(PROCESS_OP_RUN);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t process_spawn_thread(cap_id_t cap, uintptr_t entry, const void* arg_data, size_t arg_size,
                                      const char* name, size_t name_length, cap_id_t* out_thread_cap) {
	struct process_spawn_thread_request request = {
		.header      = {.op = PROCESS_OP_SPAWN_THREAD},
		.entry       = entry,
		.arg_data    = arg_data,
		.arg_size    = arg_size,
		.name        = name,
		.name_length = name_length,
	};
	struct process_spawn_thread_response response;
	syscall_result_t                     result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (entry == 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(entry);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_thread_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_thread_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((arg_data == NULL) != (arg_size == 0u)) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(arg_data);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (arg_size > THREAD_START_ARG_MAX_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(arg_size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((name == NULL) != (name_length == 0u)) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(name);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_SPAWN_THREAD, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE(
			"PROCESS_OP_SPAWN_THREAD returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	*out_thread_cap = response.thread_cap;
	if (response.thread_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_FAILED(PROCESS_OP_SPAWN_THREAD);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t process_wait(cap_id_t cap, uintptr_t* out_exit_code) {
	struct process_wait_request  request = {.header = {.op = PROCESS_OP_WAIT}};
	struct process_wait_response response;
	syscall_result_t             result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_WAIT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) {
		RUNTIME_DIAGNOSTIC_NAMED_VALUE("PROCESS_OP_WAIT returned invalid response size", "response_size", result.value);
		return SYSCALL_STATUS_FAILED;
	}
	if (out_exit_code != NULL) *out_exit_code = response.exit_code;
	return SYSCALL_STATUS_OK;
}

syscall_status_t process_detach(cap_id_t cap) {
	struct process_detach_request request = {.header = {.op = PROCESS_OP_DETACH}};
	syscall_result_t              result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_DETACH, result);
	return result.status;
}

syscall_status_t process_kill(cap_id_t cap, uintptr_t exit_code) {
	struct process_kill_request request = {.header = {.op = PROCESS_OP_KILL}, .exit_code = exit_code};
	syscall_result_t            result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(PROCESS_OP_KILL, result);
	return result.status;
}

__attribute__((noreturn))
void process_exit(uintptr_t code) {
	for (;;) {
		(void)syscall(SYSCALL_EXIT_PROCESS, code, 0u, 0u, 0u, 0u, 0u);
		(void)syscall(SYSCALL_EXIT_THREAD, code, 0u, 0u, 0u, 0u, 0u);
	}
}
