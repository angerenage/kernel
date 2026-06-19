#include <base/cap.h>
#include <base/channel.h>
#include <core/address_transfer.h>
#include <core/capability.h>
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

static syscall_result_t syscall_result_from_address_transfer(enum address_transfer_result result, uintptr_t arg_index) {
	switch (result) {
	case ADDRESS_TRANSFER_OK:
		return syscall_result_ok(0u);
	case ADDRESS_TRANSFER_FAULT_FAILED:
		return syscall_result_error(SYSCALL_STATUS_FAILED, arg_index);
	case ADDRESS_TRANSFER_INVALID_ARGUMENTS:
	case ADDRESS_TRANSFER_ADDRESS_OVERFLOW:
	case ADDRESS_TRANSFER_NOT_MAPPED:
	case ADDRESS_TRANSFER_NOT_USER:
	case ADDRESS_TRANSFER_ACCESS_DENIED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);
	}
}

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
	struct process*    process;
	struct channel*    endpoint;
	struct cap_object* object;
	struct capability* cap;
	process_id_t       caller_pid;
	process_id_t       target;
	uint64_t           object_id;
	cap_rights_t       rights;
	cap_id_t           cap_id;
	syscall_result_t   copy_result;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);
	target     = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	object_id = (uint64_t)arg2;
	rights    = (cap_rights_t)arg3;

	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	endpoint = channel_lookup((channel_id_t)arg0);
	if (endpoint == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (endpoint->owner_pid != caller_pid) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	object = cap_object_lookup(endpoint, object_id);
	if (object == NULL) {
		object = cap_object_create(object_id, endpoint);
		if (object == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	cap = cap_create(object, target, rights, NULL);
	if (cap == NULL) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	cap_id = cap->cap_id;

	if (arg4 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg4, &cap_id, sizeof(cap_id), 4u);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	}

	return syscall_result_ok((uintptr_t)cap_id);
}

syscall_result_t syscall_cap_delegate(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*    process;
	struct capability* source;
	struct capability* new_cap;
	process_id_t       caller_pid;
	process_id_t       target;
	cap_rights_t       rights;
	cap_id_t           cap_id;
	enum cap_result    auth_result;
	enum cap_result    valid_result;
	syscall_result_t   copy_result;

	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	source = cap_lookup((cap_id_t)arg0);
	if (source == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	target = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	rights = (cap_rights_t)arg2;

	auth_result = cap_is_authorized(caller_pid, source);
	if (auth_result != CAP_OK) return syscall_cap_result_to_syscall(auth_result, 0u);

	valid_result = cap_is_valid(source);
	if (valid_result != CAP_OK) return syscall_cap_result_to_syscall(valid_result, 0u);

	if ((rights & ~source->rights) != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);

	if ((source->rights & CAP_DELEGATE) == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	new_cap = cap_create(source->object, target, rights, source);
	if (new_cap == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	cap_id = new_cap->cap_id;

	if (arg3 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg3, &cap_id, sizeof(cap_id), 3u);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	}

	return syscall_result_ok((uintptr_t)cap_id);
}

syscall_result_t syscall_cap_derive(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	struct process*    process;
	struct capability* base;
	struct cap_object* object;
	struct capability* new_cap;
	process_id_t       caller_pid;
	process_id_t       target;
	uint64_t           object_id;
	cap_rights_t       rights;
	cap_id_t           cap_id;
	enum cap_result    auth_result;
	enum cap_result    valid_result;
	syscall_result_t   copy_result;

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	base = cap_lookup((cap_id_t)arg0);
	if (base == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	target = (process_id_t)arg1;
	if (target == PROCESS_PID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	object_id = (uint64_t)arg2;
	rights    = (cap_rights_t)arg3;

	auth_result = cap_is_authorized(caller_pid, base);
	if (auth_result != CAP_OK) return syscall_cap_result_to_syscall(auth_result, 0u);

	valid_result = cap_is_valid(base);
	if (valid_result != CAP_OK) return syscall_cap_result_to_syscall(valid_result, 0u);

	if (base->object->endpoint == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if ((base->rights & CAP_DERIVE) == 0u && base->object->endpoint->owner_pid != caller_pid) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	object = cap_object_lookup(base->object->endpoint, object_id);
	if (object == NULL) {
		object = cap_object_create(object_id, base->object->endpoint);
		if (object == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	new_cap = cap_create(object, target, rights, base);
	if (new_cap == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	cap_id = new_cap->cap_id;

	if (arg4 != 0u) {
		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg4, &cap_id, sizeof(cap_id), 4u);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	}

	return syscall_result_ok((uintptr_t)cap_id);
}

syscall_result_t syscall_cap_call(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct process*              process;
	struct capability*           cap;
	process_id_t                 caller_pid;
	size_t                       request_size;
	enum cap_result              auth_result;
	enum cap_result              valid_result;
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	struct cap_request           req;
	void*                        heap_request;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	caller_pid = process_pid(process);

	if (arg0 == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap = cap_lookup((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	auth_result = cap_is_authorized(caller_pid, cap);
	if (auth_result != CAP_OK) return syscall_cap_result_to_syscall(auth_result, 0u);

	valid_result = cap_is_valid(cap);
	if (valid_result != CAP_OK) return syscall_cap_result_to_syscall(valid_result, 0u);

	if ((cap->rights & CAP_CALL) == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	request_size = (size_t)arg2;
	if ((uintptr_t)request_size != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (request_size > CAP_MAX_REQUEST_SIZE) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (request_size > 0u && arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	if (cap->object->endpoint == NULL) {
		if (cap->object->handler == NULL) {
			return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		}

		req.caller       = caller_pid;
		req.cap_id       = cap->cap_id;
		req.object_id    = cap->object->object_id;
		req.rights       = cap->rights;
		req.request      = (void*)arg1;
		req.request_size = request_size;

		return cap->object->handler(&req);
	}

	heap_request = NULL;
	if (request_size > 0u) {
		heap_request = malloc(request_size);
		if (heap_request == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 2u);

		space = syscall_current_user_space();
		if (space == NULL) {
			memcpy(heap_request, (const void*)arg1, request_size);
		}
		else {
			transfer_result = address_space_copy_from(space, arg1, heap_request, request_size);
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				free(heap_request);
				return syscall_result_from_address_transfer(transfer_result, 1u);
			}
		}
	}

	req.caller       = caller_pid;
	req.cap_id       = cap->cap_id;
	req.object_id    = cap->object->object_id;
	req.rights       = cap->rights;
	req.request      = heap_request;
	req.request_size = request_size;

	if (!ring_buffer_enqueue(&cap->object->endpoint->cap_queue, &req)) {
		free(heap_request);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

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
	cap = cap_lookup((cap_id_t)arg0);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	auth_result = cap_is_authorized(caller_pid, cap);
	if (auth_result != CAP_OK) return syscall_cap_result_to_syscall(auth_result, 0u);

	if ((cap->rights & CAP_REVOKE) == 0u && cap->object->endpoint != NULL &&
	    cap->object->endpoint->owner_pid != caller_pid) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	rights = (cap_rights_t)arg1;
	if (rights == 0u) {
		cap->revoked = true;
	}
	else {
		if ((rights & ~cap->rights) != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
		cap->rights &= ~rights;
	}

	return syscall_result_ok(0u);
}

syscall_result_t syscall_cap_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	struct process*              process;
	struct channel*              endpoint;
	process_id_t                 caller_pid;
	struct cap_request           req;
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
	endpoint = channel_lookup((channel_id_t)arg0);
	if (endpoint == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (endpoint->owner_pid != caller_pid) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	if (!ring_buffer_dequeue(&endpoint->cap_queue, &req)) return syscall_result_ok(0u);

	buffer_size = (size_t)arg3;
	if ((uintptr_t)buffer_size != arg3) {
		free(req.request);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	}

	space = syscall_current_user_space();

	if (req.request_size > 0u) {
		if (arg2 == 0u || buffer_size < req.request_size) {
			free(req.request);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
		}

		if (space == NULL) {
			memcpy((void*)arg2, req.request, req.request_size);
		}
		else {
			transfer_result = address_space_copy_to(space, arg2, req.request, req.request_size);
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				free(req.request);
				return syscall_result_from_address_transfer(transfer_result, 2u);
			}
		}
	}

	free(req.request);

	{
		struct cap_request user_req;
		user_req.caller       = req.caller;
		user_req.cap_id       = req.cap_id;
		user_req.object_id    = req.object_id;
		user_req.rights       = req.rights;
		user_req.request      = (void*)arg2;
		user_req.request_size = req.request_size;

		copy_result = syscall_copy_to_user(syscall_current_user_space(), arg1, &user_req, sizeof(user_req), 1u);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	}

	return syscall_result_ok(1u);
}
