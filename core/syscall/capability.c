#include <base/cap.h>
#include <base/channel.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/channel.h>
#include <core/process.h>
#include <core/ring_buffer.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static syscall_result_t syscall_cap_result_to_syscall(enum cap_result result, uintptr_t arg_index) {
	switch (result) {
	case CAP_OK:
		return syscall_result_ok(0u);
	case CAP_INVALID_ARGUMENTS:
	case CAP_NOT_FOUND:
	case CAP_NOT_AUTHORIZED:
	case CAP_NOT_OWNER:
	case CAP_REVOKED:
	case CAP_RIGHTS_EXCEEDED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);
	case CAP_NO_MEMORY:
	case CAP_ID_EXHAUSTED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, arg_index);
	}
}

syscall_result_t syscall_cap_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct process*  process;
	struct channel*  endpoint;
	cap_object_id_t  cap_object_id;
	process_id_t     caller_pid;
	process_id_t     target;
	uint64_t         object_id;
	cap_rights_t     rights;
	cap_id_t         cap_id;
	syscall_result_t copy_result;
	bool             object_created;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);
	target     = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	object_id = (uint64_t)arg2;
	rights    = (cap_rights_t)arg3;

	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	endpoint = channel_acquire((channel_id_t)arg0);
	if (endpoint == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (endpoint->owner_pid != caller_pid) {
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap_object_id = cap_object_create(object_id, endpoint, &object_created);
	if (cap_object_id == CAP_OBJECT_ID_INVALID) {
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	cap_id = cap_create(cap_object_id, target, rights, NULL);
	if (cap_id == CAP_ID_INVALID) {
		if (object_created) (void)cap_object_destroy_with_id(cap_object_id);
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	if (arg4 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg4, &cap_id, sizeof(cap_id), 4u);
		if (copy_result.status != SYSCALL_STATUS_OK) {
			(void)cap_destroy_by_id(cap_id);
			if (object_created) (void)cap_object_destroy_with_id(cap_object_id);
			channel_release(endpoint);
			return copy_result;
		}
	}

	channel_release(endpoint);
	return syscall_result_ok((uintptr_t)cap_id);
}

syscall_result_t syscall_cap_delegate(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*    process;
	struct capability* source;
	process_id_t       caller_pid;
	process_id_t       target;
	cap_rights_t       rights;
	cap_rights_t       source_rights;
	cap_id_t           cap_id;
	enum cap_result    auth_result;
	enum cap_result    valid_result;
	syscall_result_t   copy_result;
	bool               peer;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);
	if ((arg4 & ~(uintptr_t)CAP_DELEGATE_FLAG_PEER) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 4u);
	}
	peer = (arg4 & (uintptr_t)CAP_DELEGATE_FLAG_PEER) != 0u;

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	source = cap_acquire((cap_id_t)arg0);
	if (source == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	target = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) {
		cap_release(source);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	}

	rights = (cap_rights_t)arg2;

	auth_result = cap_is_authorized(caller_pid, source);
	if (auth_result != CAP_OK) {
		cap_release(source);
		return syscall_cap_result_to_syscall(auth_result, 0u);
	}

	valid_result = cap_is_valid(source);
	if (valid_result != CAP_OK) {
		cap_release(source);
		return syscall_cap_result_to_syscall(valid_result, 0u);
	}

	source_rights = cap_rights(source);
	if ((rights & ~source_rights) != 0u) {
		cap_release(source);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}

	if ((source_rights & CAP_DELEGATE) == 0u || (peer && (source_rights & CAP_DELEGATE_PEER) == 0u)) {
		cap_release(source);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap_id = cap_delegate_create(source, target, rights, peer);
	if (cap_id == CAP_ID_INVALID) {
		cap_release(source);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	if (arg3 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg3, &cap_id, sizeof(cap_id), 3u);
		if (copy_result.status != SYSCALL_STATUS_OK) {
			(void)cap_destroy_by_id(cap_id);
			cap_release(source);
			return copy_result;
		}
	}

	cap_release(source);
	return syscall_result_ok((uintptr_t)cap_id);
}

syscall_result_t syscall_cap_derive(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct process*    process;
	struct capability* base;
	cap_object_id_t    derived_object_id;
	process_id_t       caller_pid;
	process_id_t       target;
	uint64_t           object_id;
	cap_rights_t       rights;
	cap_id_t           cap_id;
	enum cap_result    auth_result;
	enum cap_result    valid_result;
	syscall_result_t   copy_result;
	bool               object_created;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	base = cap_acquire((cap_id_t)arg0);
	if (base == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	target = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) {
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	}

	object_id = (uint64_t)arg2;
	rights    = (cap_rights_t)arg3;

	auth_result = cap_is_authorized(caller_pid, base);
	if (auth_result != CAP_OK) {
		cap_release(base);
		return syscall_cap_result_to_syscall(auth_result, 0u);
	}

	valid_result = cap_is_valid(base);
	if (valid_result != CAP_OK) {
		cap_release(base);
		return syscall_cap_result_to_syscall(valid_result, 0u);
	}

	if ((rights & ~cap_rights(base)) != 0u) {
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	}

	struct cap_object* base_object = cap_object_acquire(base->cap_object_id);
	if (base_object == NULL || base_object->endpoint == NULL) {
		cap_object_release(base_object);
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((cap_rights(base) & CAP_DERIVE) == 0u && base_object->endpoint->owner_pid != caller_pid) {
		cap_object_release(base_object);
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	derived_object_id = cap_object_create(object_id, base_object->endpoint, &object_created);
	if (derived_object_id == CAP_OBJECT_ID_INVALID) {
		cap_object_release(base_object);
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	cap_id = cap_create(derived_object_id, target, rights, base);
	cap_object_release(base_object);
	if (cap_id == CAP_ID_INVALID) {
		if (object_created) (void)cap_object_destroy_with_id(derived_object_id);
		cap_release(base);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	if (arg4 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg4, &cap_id, sizeof(cap_id), 4u);
		if (copy_result.status != SYSCALL_STATUS_OK) {
			(void)cap_destroy_by_id(cap_id);
			if (object_created) (void)cap_object_destroy_with_id(derived_object_id);
			cap_release(base);
			return copy_result;
		}
	}

	cap_release(base);
	return syscall_result_ok((uintptr_t)cap_id);
}

static syscall_result_t syscall_cap_validate_response_buffer(uintptr_t response, size_t response_capacity) {
	struct address_space*        space;
	enum address_transfer_result transfer_result;

	if (response_capacity == 0u) return syscall_result_ok(0u);

	space = syscall_current_user_space();
	if (space == NULL) return syscall_result_ok(0u);

	transfer_result = address_space_validate_range(
		space, response, response_capacity, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	return syscall_result_from_address_transfer(transfer_result, 3u);
}

syscall_result_t syscall_cap_call(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct process*    process;
	struct capability* cap;
	process_id_t       caller_pid;
	size_t             request_size;
	size_t             response_capacity;
	enum cap_result    auth_result;
	enum cap_result    valid_result;
	struct cap_object* object;
	struct cap_request req;
	void*              kernel_request;
	void*              kernel_response;
	syscall_result_t   call_result;
	cap_rights_t       granted_rights;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap = cap_acquire((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL) {
		(void)cap_destroy_by_id(cap->cap_id);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	auth_result = cap_is_authorized(caller_pid, cap);
	if (auth_result != CAP_OK) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_cap_result_to_syscall(auth_result, 0u);
	}

	valid_result = cap_is_valid(cap);
	if (valid_result != CAP_OK) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_cap_result_to_syscall(valid_result, 0u);
	}

	granted_rights = cap_rights(cap);
	if ((granted_rights & CAP_CALL) == 0u) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	request_size      = (size_t)arg2;
	response_capacity = (size_t)arg4;
	if ((uintptr_t)request_size != arg2 || request_size == 0u || request_size > CAP_MAX_REQUEST_SIZE) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}
	if (arg1 == 0u) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	}
	if ((uintptr_t)response_capacity != arg4 || response_capacity > CAP_MAX_RESPONSE_SIZE) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 4u);
	}
	if ((arg3 == 0u) != (response_capacity == 0u)) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	}

	kernel_request = malloc(request_size);
	if (kernel_request == NULL) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 2u);
	}
	call_result = syscall_copy_from_user(syscall_current_user_space(), arg1, kernel_request, request_size, 1u);
	if (call_result.status != SYSCALL_STATUS_OK) {
		free(kernel_request);
		cap_object_release(object);
		cap_release(cap);
		return call_result;
	}
	call_result = syscall_cap_validate_response_buffer(arg3, response_capacity);
	if (call_result.status != SYSCALL_STATUS_OK) {
		free(kernel_request);
		cap_object_release(object);
		cap_release(cap);
		return call_result;
	}

	if (object->endpoint == NULL) {
		if (object->handler == NULL) {
			free(kernel_request);
			cap_object_release(object);
			cap_release(cap);
			return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		}
		kernel_response = response_capacity == 0u ? NULL : malloc(response_capacity);
		if (response_capacity != 0u && kernel_response == NULL) {
			free(kernel_request);
			cap_object_release(object);
			cap_release(cap);
			return syscall_result_error(SYSCALL_STATUS_FAILED, 4u);
		}

		req.call_id           = CAP_CALL_ID_INVALID;
		req.caller            = caller_pid;
		req.cap_id            = cap->cap_id;
		req.object_id         = object->object_id;
		req.rights            = granted_rights;
		req.request           = kernel_request;
		req.request_size      = request_size;
		req.response          = kernel_response;
		req.response_capacity = response_capacity;

		call_result = object->handler(&req);
		cap_object_release(object);
		cap_release(cap);
		if (call_result.status == SYSCALL_STATUS_OK && call_result.value > response_capacity) {
			free(kernel_response);
			free(kernel_request);
			return syscall_result_error(SYSCALL_STATUS_FAILED, 4u);
		}
		if (call_result.status == SYSCALL_STATUS_OK && call_result.value != 0u) {
			syscall_result_t copy_result = syscall_copy_to_user(
				syscall_current_user_space(), arg3, kernel_response, (size_t)call_result.value, 3u);
			if (copy_result.status != SYSCALL_STATUS_OK) call_result = copy_result;
		}
		free(kernel_response);
		free(kernel_request);
		return call_result;
	}

	struct cap_pending_call* pending =
		cap_pending_call_create(object->endpoint->id, object->endpoint->owner_pid, caller_pid, response_capacity);
	if (pending == NULL) {
		free(kernel_request);
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	req.call_id           = cap_pending_call_id(pending);
	req.caller            = caller_pid;
	req.cap_id            = cap->cap_id;
	req.object_id         = object->object_id;
	req.rights            = granted_rights;
	req.request           = kernel_request;
	req.request_size      = request_size;
	req.response          = NULL;
	req.response_capacity = response_capacity;
	if (!cap_pending_call_attach_request(pending, kernel_request, request_size)) {
		cap_pending_call_destroy(pending);
		free(kernel_request);
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	kernel_request = NULL;

	if (!channel_enqueue_cap_request(object->endpoint, &req)) {
		cap_pending_call_destroy(pending);
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	cap_object_release(object);
	cap_release(cap);
	const void* pending_response = NULL;
	cap_pending_call_wait(pending, &call_result, &pending_response);
	if (call_result.status == SYSCALL_STATUS_OK && call_result.value != 0u) {
		syscall_result_t copy_result =
			syscall_copy_to_user(syscall_current_user_space(), arg3, pending_response, (size_t)call_result.value, 3u);
		if (copy_result.status != SYSCALL_STATUS_OK) call_result = copy_result;
	}
	cap_pending_call_destroy(pending);
	return call_result;
}

static bool cap_reply_status_is_valid(syscall_status_t status) {
	switch (status) {
	case SYSCALL_STATUS_OK:
	case SYSCALL_STATUS_BAD_ARGUMENT:
	case SYSCALL_STATUS_DENIED:
	case SYSCALL_STATUS_FAILED:
	case SYSCALL_STATUS_UNAVAILABLE:
		return true;
	case SYSCALL_STATUS_UNKNOWN_SYSCALL:
	default:
		return false;
	}
}

syscall_result_t syscall_cap_reply(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                   uintptr_t arg5) {
	struct process*               process;
	struct cap_pending_reply      reply;
	size_t                        response_size;
	syscall_status_t              status = (syscall_status_t)arg3;
	enum cap_pending_reply_result prepare_result;
	syscall_result_t              copy_result;

	(void)arg4;
	(void)arg5;
	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CAP_CALL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response_size = (size_t)arg2;
	if ((uintptr_t)response_size != arg2 || response_size > CAP_MAX_RESPONSE_SIZE) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}
	if ((arg1 == 0u) != (response_size == 0u)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (!cap_reply_status_is_valid(status)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if (status != SYSCALL_STATUS_OK && response_size != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}

	prepare_result = cap_pending_call_prepare_reply((cap_call_id_t)arg0, process_pid(process), response_size, &reply);
	switch (prepare_result) {
	case CAP_PENDING_REPLY_OK:
		break;
	case CAP_PENDING_REPLY_NOT_OWNER:
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	case CAP_PENDING_REPLY_TOO_LARGE:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	case CAP_PENDING_REPLY_NOT_FOUND:
	case CAP_PENDING_REPLY_ALREADY_COMPLETED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	copy_result = syscall_copy_from_user(syscall_current_user_space(), arg1, reply.response, response_size, 1u);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		cap_pending_call_abort_reply(&reply);
		return copy_result;
	}
	cap_pending_call_finish_reply(&reply, status, response_size);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_cap_revoke(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct process*    process;
	struct capability* cap;
	process_id_t       caller_pid;
	cap_rights_t       rights;
	enum cap_result    auth_result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap = cap_acquire((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	auth_result = cap_is_authorized(caller_pid, cap);
	if (auth_result != CAP_OK) {
		cap_release(cap);
		return syscall_cap_result_to_syscall(auth_result, 0u);
	}

	struct cap_object* revoke_object = cap_object_acquire(cap->cap_object_id);
	if (revoke_object == NULL) {
		(void)cap_destroy_by_id(cap->cap_id);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((cap_rights(cap) & CAP_REVOKE) == 0u && revoke_object->endpoint != NULL &&
	    revoke_object->endpoint->owner_pid != caller_pid) {
		cap_object_release(revoke_object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	cap_object_release(revoke_object);

	rights = (cap_rights_t)arg1;
	if (rights == 0u) {
		if (!cap_destroy(cap)) {
			cap_release(cap);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		}
	}
	else {
		if (!cap_remove_rights(cap, rights)) {
			cap_release(cap);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
		}
	}

	cap_release(cap);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_cap_drop(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct process*    process;
	struct capability* cap;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	cap = cap_acquire((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (cap->target != process_pid(process) || !cap_drop(cap)) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap_release(cap);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_cap_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct process*              process;
	struct channel*              endpoint;
	process_id_t                 caller_pid;
	struct cap_request           req;
	struct cap_pending_request   pending_request = {0};
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	syscall_result_t             copy_result;
	size_t                       buffer_size;

	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	endpoint = channel_acquire((channel_id_t)arg0);
	if (endpoint == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (endpoint->owner_pid != caller_pid) {
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if (arg1 == 0u) {
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	}

	for (;;) {
		if (!ring_buffer_dequeue(&endpoint->cap_queue, &req)) {
			channel_release(endpoint);
			return syscall_result_ok(0u);
		}
		if (cap_pending_call_prepare_receive(req.call_id, caller_pid, &pending_request)) break;
	}
	req.request      = pending_request.request;
	req.request_size = pending_request.request_size;

	buffer_size = (size_t)arg3;
	if ((uintptr_t)buffer_size != arg3) {
		(void)cap_pending_call_fail(req.call_id, SYSCALL_STATUS_FAILED);
		cap_pending_call_finish_receive(&pending_request);
		channel_release(endpoint);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	}

	space = syscall_current_user_space();

	if (req.request_size > 0u) {
		if (arg2 == 0u || buffer_size < req.request_size) {
			(void)cap_pending_call_fail(req.call_id, SYSCALL_STATUS_FAILED);
			cap_pending_call_finish_receive(&pending_request);
			channel_release(endpoint);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
		}

		if (space == NULL) {
			memcpy((void*)arg2, req.request, req.request_size);
		}
		else {
			transfer_result = address_space_copy_to(space, arg2, req.request, req.request_size);
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				(void)cap_pending_call_fail(req.call_id, SYSCALL_STATUS_FAILED);
				cap_pending_call_finish_receive(&pending_request);
				channel_release(endpoint);
				return syscall_result_from_address_transfer(transfer_result, 2u);
			}
		}
	}

	{
		struct cap_request user_req;
		user_req.call_id           = req.call_id;
		user_req.caller            = req.caller;
		user_req.cap_id            = req.cap_id;
		user_req.object_id         = req.object_id;
		user_req.rights            = req.rights;
		user_req.request           = (const void*)arg2;
		user_req.request_size      = req.request_size;
		user_req.response          = NULL;
		user_req.response_capacity = req.response_capacity;

		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg1, &user_req, sizeof(user_req), 1u);
		if (copy_result.status != SYSCALL_STATUS_OK) {
			(void)cap_pending_call_fail(req.call_id, SYSCALL_STATUS_FAILED);
			cap_pending_call_finish_receive(&pending_request);
			channel_release(endpoint);
			return copy_result;
		}
	}

	cap_pending_call_finish_receive(&pending_request);
	channel_release(endpoint);
	return syscall_result_ok(1u);
}

syscall_result_t syscall_cap_valid(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                   uintptr_t arg5) {
	struct process*    process;
	struct capability* cap;
	bool               valid;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	cap = cap_acquire((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_ok(0u);

	/* Do not disclose whether another process owns a live capability ID. */
	valid = cap->target == process_pid(process) && cap_is_valid(cap) == CAP_OK;
	cap_release(cap);
	return syscall_result_ok(valid ? 1u : 0u);
}
