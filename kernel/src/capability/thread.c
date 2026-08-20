#include "thread.h"

#include <base/cap.h>
#include <base/syscall.h>
#include <base/thread.h>
#include <core/capability.h>
#include <core/process.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <kernel/capability.h>
#include <string.h>

static enum thread_op thread_op_from_request(const void* request, size_t request_size) {
	if (request == NULL || request_size < sizeof(struct thread_request_header)) return (enum thread_op) - 1;
	return ((const struct thread_request_header*)request)->op;
}

static syscall_result_t thread_join_handler(const struct cap_request* req, struct uthread* target) {
	struct thread_join_response     response;
	enum process_thread_join_result result;
	struct process*                 process;

	if (!cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	process = target->process;
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	result = process_join_thread(process, target, &response.exit_code);
	switch (result) {
	case PROCESS_THREAD_JOIN_OK:
		return cap_kernel_write_response(req, &response, sizeof(response));
	case PROCESS_THREAD_JOIN_INVALID_ARGUMENTS:
	case PROCESS_THREAD_JOIN_FOREIGN_THREAD:
	case PROCESS_THREAD_JOIN_SELF:
	case PROCESS_THREAD_JOIN_DETACHED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case PROCESS_THREAD_JOIN_WAIT_FAILED:
	case PROCESS_THREAD_JOIN_RECLAIM_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t thread_detach_handler(struct uthread* target) {
	enum process_thread_detach_result result;

	if (target->process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = process_detach_thread(target->process, target);
	switch (result) {
	case PROCESS_THREAD_DETACH_OK:
		return syscall_result_ok(0u);
	case PROCESS_THREAD_DETACH_INVALID_ARGUMENTS:
	case PROCESS_THREAD_DETACH_FOREIGN_THREAD:
	case PROCESS_THREAD_DETACH_ALREADY_DETACHED:
	case PROCESS_THREAD_DETACH_ALREADY_TERMINATED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case PROCESS_THREAD_DETACH_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t thread_cancel_handler(struct uthread* target) {
	enum process_thread_cancel_result result;

	if (target->process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = process_cancel_thread(target->process, target);
	switch (result) {
	case PROCESS_THREAD_CANCEL_OK:
		return syscall_result_ok(0u);
	case PROCESS_THREAD_CANCEL_INVALID_ARGUMENTS:
	case PROCESS_THREAD_CANCEL_FOREIGN_THREAD:
	case PROCESS_THREAD_CANCEL_ALREADY_TERMINATED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case PROCESS_THREAD_CANCEL_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t thread_set_cancel_enabled_handler(const struct cap_request* req, struct uthread* target) {
	struct thread_set_cancel_enabled_request request;

	if (req->request == NULL || req->request_size < sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&request, req->request, sizeof(request));
	thread_set_cancel_enabled(&target->thread, request.enabled);
	return syscall_result_ok(0u);
}

static syscall_result_t thread_test_cancel_handler(const struct cap_request* req, struct uthread* target) {
	struct thread_test_cancel_response response;

	if (!cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	response.should_cancel = thread_should_cancel(&target->thread);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t thread_handler(const struct cap_request* req) {
	struct uthread*  target;
	enum thread_op   op;
	cap_rights_t     required_rights;
	syscall_result_t result;

	target = uthread_acquire((uthread_id_t)req->object_id);
	if (target == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	op = thread_op_from_request(req->request, req->request_size);
	if ((int)op < 0) {
		uthread_release(target);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	switch (op) {
	case THREAD_OP_JOIN:
		required_rights = CAP_WAIT;
		break;
	case THREAD_OP_DETACH:
	case THREAD_OP_SET_CANCEL_ENABLED:
		required_rights = CAP_MANAGE;
		break;
	case THREAD_OP_CANCEL:
		required_rights = CAP_DESTROY;
		break;
	case THREAD_OP_TEST_CANCEL:
		required_rights = CAP_READ;
		break;
	default:
		uthread_release(target);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((req->rights & required_rights) != required_rights) {
		uthread_release(target);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (op) {
	case THREAD_OP_JOIN:
		result = thread_join_handler(req, target);
		break;
	case THREAD_OP_DETACH:
		result = thread_detach_handler(target);
		break;
	case THREAD_OP_CANCEL:
		result = thread_cancel_handler(target);
		break;
	case THREAD_OP_SET_CANCEL_ENABLED:
		result = thread_set_cancel_enabled_handler(req, target);
		break;
	case THREAD_OP_TEST_CANCEL:
		result = thread_test_cancel_handler(req, target);
		break;
	default:
		result = syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		break;
	}

	uthread_release(target);
	return result;
}

cap_id_t kernel_thread_grant(struct uthread* target, process_id_t recipient, cap_rights_t rights) {
	struct cap_object* object = NULL;
	cap_object_id_t    object_id;
	uthread_id_t       thread_id;
	struct uthread*    held;
	cap_id_t           result_cap     = CAP_ID_INVALID;
	bool               object_created = false;

	if (target == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	thread_id = uthread_id(target);
	held      = uthread_acquire(thread_id);
	if (held != target) {
		if (held != NULL) uthread_release(held);
		return CAP_ID_INVALID;
	}

	object_id = uthread_cap_object_id(held);
	if (object_id != CAP_OBJECT_ID_INVALID) object = cap_object_acquire(object_id);
	if (object == NULL) {
		object_id = cap_object_create_kernel((uint64_t)thread_id, thread_handler, &object_created);
		if (object_id == CAP_OBJECT_ID_INVALID) goto out;
		uthread_set_cap_object_id(held, object_id);
	}
	else {
		cap_object_release(object);
	}

	result_cap = cap_create(object_id, recipient, rights, NULL);
	if (result_cap == CAP_ID_INVALID && object_created) {
		uthread_set_cap_object_id(held, CAP_OBJECT_ID_INVALID);
		(void)cap_object_destroy_with_id(object_id);
	}
out:
	uthread_release(held);
	return result_cap;
}

cap_id_t kernel_thread_grant_full(struct uthread* target, process_id_t recipient) {
	return kernel_thread_grant(
		target, recipient, CAP_CALL | CAP_WAIT | CAP_MANAGE | CAP_READ | CAP_DESTROY | CAP_DELEGATE);
}
