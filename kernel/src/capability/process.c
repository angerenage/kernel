#include "process.h"

#include <base/cap.h>
#include <base/self.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/message.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <stdlib.h>
#include <string.h>

/* Extract the operation code from a request buffer. Returns -1 if the buffer is too small. */
static enum process_op process_op_from_request(const void* request, size_t request_size) {
	if (request == NULL || request_size < sizeof(struct process_request_header)) {
		return (enum process_op) - 1;
	}
	return (enum process_op)((const struct process_request_header*)request)->op;
}

static syscall_result_t process_info_handler(const struct cap_request* req, struct process* target) {
	struct process*              caller;
	struct process_info_response response;
	enum message_result          result;

	response.pid          = process_pid(target);
	response.thread_id    = 0u;
	response.thread_count = process_thread_count(target);

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	result = message_queue_send(&caller->message_queue, req->caller, &response, sizeof(response));
	if (result != MESSAGE_OK) return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);

	return syscall_result_ok(0u);
}

static syscall_result_t process_run_handler(const struct cap_request* req, struct process* target) {
	struct process_run_request       request;
	enum address_transfer_result     transfer_result;
	struct process*                  caller;
	struct uthread*                  main_thread = NULL;
	enum process_thread_spawn_result result;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	transfer_result =
		address_space_copy_from(process_address_space(caller), (uintptr_t)req->request, &request, sizeof(request));
	if (transfer_result != ADDRESS_TRANSFER_OK) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	result = process_start_main_thread(target,
	                                   &main_thread,
	                                   &(const struct process_thread_params){
										   .name             = target->name,
										   .user_entry       = request.entry,
										   .user_arg         = request.arg,
										   .user_stack_pages = request.stack_pages,
										   .preferred_cpu    = NULL,
										   .detached         = false,
									   });
	if (result != PROCESS_THREAD_SPAWN_OK) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}

	return syscall_result_ok((uintptr_t)uthread_id(main_thread));
}

static syscall_result_t process_wait_handler(const struct cap_request* req, struct process* target) {
	struct process*              caller;
	struct process_wait_response response;
	enum process_join_result     join_result;
	uintptr_t                    exit_code = 0u;

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

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return message_queue_send(&caller->message_queue, req->caller, &response, sizeof(response)) == MESSAGE_OK
	           ? syscall_result_ok(0u)
	           : syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
}

static syscall_result_t process_detach_handler(const struct cap_request* req, struct process* target) {
	enum process_detach_result detach_result;

	(void)req;

	detach_result = process_detach(target);
	switch (detach_result) {
	case PROCESS_DETACH_OK:
		return syscall_result_ok(0u);
	case PROCESS_DETACH_INVALID_ARGUMENTS:
	case PROCESS_DETACH_ALREADY_DETACHED:
	case PROCESS_DETACH_ALREADY_JOINED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)detach_result);
	}
}

static syscall_result_t process_kill_handler(const struct cap_request* req, struct process* target) {
	struct process*              caller;
	struct process_kill_request  request;
	enum address_transfer_result transfer_result;
	bool                         success;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	transfer_result =
		address_space_copy_from(process_address_space(caller), (uintptr_t)req->request, &request, sizeof(request));
	if (transfer_result != ADDRESS_TRANSFER_OK) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	success = process_terminate(target, request.exit_code);
	if (!success) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok(0u);
}

static syscall_result_t process_handler(const struct cap_request* req) {
	struct process*  process;
	enum process_op  op;
	cap_rights_t     required_rights;
	void*            kernel_request = NULL;
	syscall_result_t result;

	process = (struct process*)(uintptr_t)req->object_id;
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	if (req->request_size == 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	kernel_request = malloc(req->request_size);
	if (kernel_request == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	{
		struct process*       caller;
		struct address_space* space;

		caller = process_lookup(req->caller);
		if (caller == NULL) {
			free(kernel_request);
			return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
		}

		space = process_address_space(caller);
		if (space != NULL) {
			enum address_transfer_result transfer_result;
			transfer_result =
				address_space_copy_from(space, (uintptr_t)req->request, kernel_request, req->request_size);
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				free(kernel_request);
				return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
			}
		}
		else {
			memcpy(kernel_request, req->request, req->request_size);
		}
	}

	op = process_op_from_request(kernel_request, req->request_size);
	if ((int)op < 0) {
		free(kernel_request);
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
		required_rights = CAP_EXEC;
		break;
	default:
		free(kernel_request);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((req->rights & required_rights) != required_rights) {
		free(kernel_request);
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
	default:
		result = syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		break;
	}

	free(kernel_request);
	return result;
}

cap_id_t kernel_process_grant(struct process* target, cap_rights_t rights) {
	struct cap_object* object;
	struct capability* cap;

	if (target == NULL) return CAP_ID_INVALID;

	object = cap_object_create_kernel((uint64_t)(uintptr_t)target, process_handler);
	if (object == NULL) return CAP_ID_INVALID;

	cap = cap_create(object, process_pid(target), rights, NULL);
	if (cap == NULL) {
		cap_object_destroy(object);
		return CAP_ID_INVALID;
	}

	return cap->cap_id;
}

cap_id_t kernel_self_grant(struct process* process) {
	if (process == NULL) return CAP_ID_INVALID;
	if (process->self_cap != CAP_ID_INVALID) return process->self_cap;

	process->self_cap = kernel_process_grant(
		process, CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_EXEC | CAP_DELEGATE);
	return process->self_cap;
}
