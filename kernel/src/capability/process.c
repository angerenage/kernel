#include "process.h"

#include <base/cap.h>
#include <base/process.h>
#include <base/thread.h>
#include <core/capability.h>
#include <core/cpu.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <string.h>

#include "thread.h"

/* Extract the operation code from a request buffer. Returns -1 if the buffer is too small. */
static enum process_op process_op_from_request(const void* request, size_t request_size) {
	if (request == NULL || request_size < sizeof(struct process_request_header)) {
		return (enum process_op) - 1;
	}
	return (enum process_op)((const struct process_request_header*)request)->op;
}

static syscall_result_t process_info_handler(const struct cap_request* req, struct process* target) {
	struct process_info_response response;

	response.pid          = process_pid(target);
	response.thread_id    = 0u;
	response.thread_count = process_thread_count(target);

	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t process_copy_thread_arg(const struct cap_request* req, const void* arg_data, size_t arg_size,
                                                void** out_arg) {
	struct process*  caller;
	void*            copy;
	syscall_result_t result;

	if (out_arg == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	*out_arg = NULL;
	if ((arg_data == NULL) != (arg_size == 0u) || arg_size > THREAD_START_ARG_MAX_SIZE) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (arg_size == 0u) return syscall_result_ok(0u);

	caller = process_acquire(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	copy = malloc(arg_size);
	if (copy == NULL) {
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	result = syscall_copy_from_user(process_address_space(caller), (uintptr_t)arg_data, copy, arg_size, 0u);
	process_release(caller);
	if (result.status != SYSCALL_STATUS_OK) {
		free(copy);
		return result;
	}
	*out_arg = copy;
	return syscall_result_ok(0u);
}

static syscall_result_t process_run_handler(const struct cap_request* req, struct process* target) {
	struct process_run_request       request;
	struct uthread*                  main_thread = NULL;
	enum process_thread_spawn_result result;
	void*                            arg_copy           = NULL;
	bool                             thread_cap_created = false;
	syscall_result_t                 copy_result;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (!cap_kernel_response_fits(req, sizeof(struct process_run_response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	memcpy(&request, req->request, sizeof(request));
	copy_result = process_copy_thread_arg(req, request.arg_data, request.arg_size, &arg_copy);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	result = process_prepare_main_thread(target,
	                                     &main_thread,
	                                     &(const struct process_thread_params){
											 .name             = target->name,
											 .user_entry       = request.entry,
											 .arg_data         = arg_copy,
											 .arg_size         = request.arg_size,
											 .user_stack_pages = UTHREAD_DEFAULT_USER_STACK_PAGES,
											 .preferred_cpu    = cpu_current(),
											 .detached         = false,
										 });
	free(arg_copy);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}

	struct process_run_response response = {
		.thread_id  = uthread_id(main_thread),
		.thread_cap = kernel_thread_grant_full(main_thread, req->caller, &thread_cap_created),
	};
	if (response.thread_cap == CAP_ID_INVALID) {
		(void)process_abort_main_thread(target, main_thread);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	result = process_commit_main_thread(target, main_thread);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		if (thread_cap_created) (void)cap_destroy_by_id(response.thread_cap);
		(void)process_abort_main_thread(target, main_thread);
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t process_spawn_thread_handler(const struct cap_request* req, struct process* target) {
	struct process_spawn_thread_request  request;
	struct process_spawn_thread_response response;
	struct process_thread_params         params;
	struct uthread*                      thread = NULL;
	enum process_thread_spawn_result     result;
	bool                                 thread_cap_created = false;
	char*                                name               = NULL;
	void*                                arg_copy           = NULL;
	syscall_result_t                     copy_result;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (!cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&request, req->request, sizeof(request));
	if (request.entry == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	copy_result = process_copy_thread_arg(req, request.arg_data, request.arg_size, &arg_copy);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	copy_result = syscall_copy_string_arg(0u, (uintptr_t)request.name, 0u, (uintptr_t)request.name_length, &name);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		free(arg_copy);
		return copy_result;
	}

	params = (struct process_thread_params){
		.name             = name,
		.user_entry       = request.entry,
		.arg_data         = arg_copy,
		.arg_size         = request.arg_size,
		.user_stack_pages = UTHREAD_DEFAULT_USER_STACK_PAGES,
		.preferred_cpu    = NULL,
		.detached         = false,
	};
	result = process_prepare_thread(target, &thread, &params);
	free(name);
	free(arg_copy);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		return result == PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS
		           ? syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result)
		           : syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}

	response.thread_cap = kernel_thread_grant_full(thread, req->caller, &thread_cap_created);
	if (response.thread_cap == CAP_ID_INVALID) {
		(void)process_abort_thread(target, thread);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	result = process_commit_thread(target, thread);
	if (result != PROCESS_THREAD_SPAWN_OK) {
		if (thread_cap_created) (void)cap_destroy_by_id(response.thread_cap);
		(void)process_abort_thread(target, thread);
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t process_wait_handler(const struct cap_request* req, struct process* target) {
	struct process_wait_response response;
	enum process_join_result     join_result;
	uintptr_t                    exit_code = 0u;

	if (!cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	join_result = process_join(target, &exit_code);
	switch (join_result) {
	case PROCESS_JOIN_OK:
		process_destroy(target);
		response.exit_code = exit_code;
		break;
	case PROCESS_JOIN_INVALID_ARGUMENTS:
	case PROCESS_JOIN_SELF:
	case PROCESS_JOIN_DETACHED:
	case PROCESS_JOIN_ALREADY_JOINED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_JOIN_WAIT_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)join_result);
	}

	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t process_detach_handler(const struct cap_request* req, struct process* target) {
	enum process_detach_result detach_result;

	(void)req;

	detach_result = process_detach(target);
	switch (detach_result) {
	case PROCESS_DETACH_OK:
		/* If the process was already a zombie, no later thread callback exists to trigger detached reaping. */
		(void)process_reap_detached(target);
		return syscall_result_ok(0u);
	case PROCESS_DETACH_INVALID_ARGUMENTS:
	case PROCESS_DETACH_ALREADY_DETACHED:
	case PROCESS_DETACH_ALREADY_JOINED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)detach_result);
	}
}

static syscall_result_t process_kill_handler(const struct cap_request* req, struct process* target) {
	struct process_kill_request request;
	bool                        success;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	memcpy(&request, req->request, sizeof(request));

	success = process_terminate(target, request.exit_code);
	if (!success) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok(0u);
}

static syscall_result_t process_handler(const struct cap_request* req) {
	struct process*  process;
	enum process_op  op;
	cap_rights_t     required_rights;
	syscall_result_t result;

	process = process_acquire((process_id_t)req->object_id);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	if (req->request_size == 0u) {
		process_release(process);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	op = process_op_from_request(req->request, req->request_size);
	if ((int)op < 0) {
		process_release(process);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	switch (op) {
	case PROCESS_OP_INFO:
		required_rights = CAP_READ;
		break;
	case PROCESS_OP_WAIT:
		required_rights = CAP_WAIT;
		break;
	case PROCESS_OP_DETACH:
		required_rights = CAP_MANAGE;
		break;
	case PROCESS_OP_KILL:
		required_rights = CAP_DESTROY;
		break;
	case PROCESS_OP_RUN:
	case PROCESS_OP_SPAWN_THREAD:
		required_rights = CAP_EXEC;
		break;
	default:
		process_release(process);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((req->rights & required_rights) != required_rights) {
		process_release(process);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (op) {
	case PROCESS_OP_INFO:
		result = process_info_handler(req, process);
		break;
	case PROCESS_OP_WAIT:
		result = process_wait_handler(req, process);
		break;
	case PROCESS_OP_DETACH:
		result = process_detach_handler(req, process);
		break;
	case PROCESS_OP_KILL:
		result = process_kill_handler(req, process);
		break;
	case PROCESS_OP_RUN:
		result = process_run_handler(req, process);
		break;
	case PROCESS_OP_SPAWN_THREAD:
		result = process_spawn_thread_handler(req, process);
		break;
	default:
		result = syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		break;
	}

	process_release(process);
	return result;
}

cap_id_t kernel_process_grant(struct process* target, process_id_t recipient, cap_rights_t rights, bool* out_created) {
	struct cap_object* object;
	cap_object_id_t    object_id;
	bool               object_created = false;

	if (out_created != NULL) *out_created = false;
	if (target == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;

	object_id = process_cap_object_id(target);
	if (object_id != CAP_OBJECT_ID_INVALID) {
		object = cap_object_acquire(object_id);
		if (object == NULL) {
			object_id = cap_object_create_kernel((uint64_t)process_pid(target), process_handler, &object_created);
			if (object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
			process_set_cap_object_id(target, object_id);
		}
		else {
			cap_object_release(object);
		}
	}
	else {
		object_id = cap_object_create_kernel((uint64_t)process_pid(target), process_handler, &object_created);
		if (object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
		process_set_cap_object_id(target, object_id);
	}

	cap_id_t cap_id = cap_create(object_id, recipient, rights, NULL, out_created);
	if (cap_id == CAP_ID_INVALID) {
		if (object_created) {
			process_set_cap_object_id(target, CAP_OBJECT_ID_INVALID);
			(void)cap_object_destroy_with_id(object_id);
		}
		return CAP_ID_INVALID;
	}

	return cap_id;
}

cap_id_t kernel_self_grant(struct process* process, bool* out_created) {
	return kernel_process_grant(process,
	                            process_pid(process),
	                            CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_EXEC | CAP_DELEGATE,
	                            out_created);
}
