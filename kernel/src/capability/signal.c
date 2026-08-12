#include "signal.h"

#include <base/cap.h>
#include <base/signal.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/signal.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <kernel/capability.h>
#include <string.h>

struct kernel_signal_reference {
	struct cap_object* object;
	struct signal*     signal;
};

static enum signal_op signal_op_from_request(const void* request, size_t request_size) {
	if (request == NULL || request_size < sizeof(struct signal_request_header)) return (enum signal_op) - 1;
	return ((const struct signal_request_header*)request)->op;
}

static syscall_result_t signal_result_to_syscall(enum signal_result result) {
	switch (result) {
	case SIGNAL_OK:
		return syscall_result_ok(0u);
	case SIGNAL_INVALID_ARGUMENTS:
	case SIGNAL_WAIT_RECEIVER_NOT_REGISTERED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case SIGNAL_NOT_FOUND:
	case SIGNAL_CLOSED:
	case SIGNAL_UNAVAILABLE:
	case SIGNAL_WAIT_CANCELED:
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, (uintptr_t)result);
	case SIGNAL_WAIT_INTERRUPTED:
		return syscall_result_error(SYSCALL_STATUS_INTERRUPTED, (uintptr_t)result);
	case SIGNAL_NO_VALUE:
	case SIGNAL_WOULD_BLOCK:
	case SIGNAL_HANDLER_NOT_REGISTERED:
	case SIGNAL_HANDLER_ALREADY_REGISTERED:
	case SIGNAL_NO_MEMORY:
	case SIGNAL_WAIT_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t signal_set_handler(const struct cap_request* req, struct signal* signal) {
	const struct signal_set_handler_request* request;
	struct uthread*                          current;

	if (req->request_size != sizeof(*request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	request = req->request;
	if (request->handler == NULL || (request->flags & ~(uint32_t)SIGNAL_HANDLER_FLAG_ONESHOT) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	current = uthread_current();
	if (current == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return signal_result_to_syscall(signal_register_handler(signal, current, request->handler, request->flags));
}

static syscall_result_t signal_clear_handler(const struct cap_request* req, struct signal* signal) {
	struct uthread* current;

	if (req->request_size != sizeof(struct signal_clear_handler_request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	current = uthread_current();
	if (current == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return signal_result_to_syscall(signal_unregister_handler(signal, current));
}

static syscall_result_t signal_read_info(const struct cap_request* req, struct signal* signal) {
	struct signal_read_response response;
	struct uthread*             current;

	if (req->request_size != sizeof(struct signal_read_request) || req->response == NULL ||
	    req->response_capacity < sizeof(response)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	current  = uthread_current();
	response = (struct signal_read_response){
		.generation                  = signal_generation(signal),
		.handler_count               = (uint64_t)signal_handler_count(signal),
		.wait_subscription_count     = (uint64_t)signal_wait_subscription_count(signal),
		.blocked_waiter_count        = (uint64_t)signal_blocked_waiter_count(signal),
		.caller_upcall_pending_count = (uint64_t)uthread_upcall_pending_count(current),
		.caller_upcall_dropped_count = uthread_upcall_dropped_count(current),
		.caller_upcall_capacity      = USER_UPCALL_QUEUE_CAPACITY,
		.flags                       = signal_has_value(signal) ? SIGNAL_READ_FLAG_HAS_VALUE : SIGNAL_READ_FLAG_NONE,
	};
	memcpy(req->response, &response, sizeof(response));
	return syscall_result_ok(sizeof(response));
}

static syscall_result_t signal_handler(const struct cap_request* req) {
	struct signal*   signal;
	enum signal_op   op;
	cap_rights_t     required_rights;
	syscall_result_t result;

	if (req == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	signal = signal_acquire((signal_id_t)req->object_id);
	if (signal == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	op = signal_op_from_request(req->request, req->request_size);
	if ((int)op < 0) {
		signal_release(signal);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	switch (op) {
	case SIGNAL_OP_UNSUBSCRIBE:
		required_rights = CAP_WAIT;
		break;
	case SIGNAL_OP_DESTROY:
		required_rights = CAP_DESTROY;
		break;
	case SIGNAL_OP_SET_HANDLER:
	case SIGNAL_OP_CLEAR_HANDLER:
		required_rights = CAP_MAP;
		break;
	case SIGNAL_OP_READ:
		required_rights = CAP_READ;
		break;
	default:
		signal_release(signal);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((req->rights & required_rights) != required_rights) {
		signal_release(signal);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (op) {
	case SIGNAL_OP_UNSUBSCRIBE:
		result = signal_result_to_syscall(signal_unregister_wait_receiver(signal));
		break;
	case SIGNAL_OP_DESTROY:
		result = signal_result_to_syscall(signal_destroy(signal));
		break;
	case SIGNAL_OP_SET_HANDLER:
		result = signal_set_handler(req, signal);
		break;
	case SIGNAL_OP_CLEAR_HANDLER:
		result = signal_clear_handler(req, signal);
		break;
	case SIGNAL_OP_READ:
		result = signal_read_info(req, signal);
		break;
	default:
		result = syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		break;
	}

	signal_release(signal);
	return result;
}

static syscall_result_t kernel_signal_acquire(cap_id_t cap_id, process_id_t caller, cap_rights_t required_rights,
                                              struct kernel_signal_reference* out_reference) {
	struct cap_object* object;
	struct capability* cap;
	struct signal*     signal;
	enum cap_result    cap_result;

	if (out_reference == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	*out_reference = (struct kernel_signal_reference){0};
	if (cap_id == CAP_ID_INVALID || caller == PROCESS_PID_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap = cap_acquire(cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap_result = cap_is_authorized(caller, cap);
	if (cap_result == CAP_OBJECT_DESTROYED) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	if (cap_result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	cap_result = cap_is_valid(cap);
	if (cap_result == CAP_OBJECT_DESTROYED || cap_result == CAP_NOT_FOUND) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	if (cap_result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	if ((cap_rights(cap) & required_rights) != required_rights) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	if (object->endpoint != NULL || object->handler != signal_handler) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	signal = signal_acquire((signal_id_t)object->object_id);
	if (signal == NULL) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}

	out_reference->object = object;
	out_reference->signal = signal;
	cap_release(cap);
	return syscall_result_ok(0u);
}

static void kernel_signal_release(struct kernel_signal_reference* reference) {
	if (reference == NULL) return;
	signal_release(reference->signal);
	cap_object_release(reference->object);
	*reference = (struct kernel_signal_reference){0};
}

syscall_result_t kernel_signal_send(cap_id_t cap, process_id_t caller, const struct signal_payload* payload,
                                    uint32_t flags, struct signal_send_response* out_response) {
	struct kernel_signal_reference reference;
	struct signal_send_response    response = {0};
	syscall_result_t               result;
	enum signal_result             signal_result;

	if (payload == NULL || (flags & ~((uint32_t)SIGNAL_SEND_FLAG_COALESCE)) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	result = kernel_signal_acquire(cap, caller, CAP_SIGNAL, &reference);
	if (result.status != SYSCALL_STATUS_OK) return result;
	if ((flags & (uint32_t)SIGNAL_SEND_FLAG_COALESCE) != 0u) {
		signal_result = signal_send_coalesced(
			reference.signal, caller, payload, &response.receiver_count, &response.delivery_count);
	}
	else {
		signal_result =
			signal_send(reference.signal, caller, payload, &response.receiver_count, &response.delivery_count);
	}
	kernel_signal_release(&reference);
	if (signal_result != SIGNAL_OK) return signal_result_to_syscall(signal_result);
	if (out_response != NULL) *out_response = response;
	return syscall_result_ok(0u);
}

syscall_result_t kernel_signal_read(cap_id_t cap, process_id_t caller, struct signal_message* out_message) {
	struct kernel_signal_reference reference;
	syscall_result_t               result;
	enum signal_result             signal_result;

	if (out_message == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	result = kernel_signal_acquire(cap, caller, CAP_READ, &reference);
	if (result.status != SYSCALL_STATUS_OK) return result;
	signal_result = signal_read(reference.signal, out_message);
	kernel_signal_release(&reference);
	if (signal_result == SIGNAL_NO_VALUE) return syscall_result_ok(0u);
	if (signal_result != SIGNAL_OK) return signal_result_to_syscall(signal_result);
	return syscall_result_ok(1u);
}

syscall_result_t kernel_signal_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message) {
	struct kernel_signal_reference reference;
	syscall_result_t               result;
	enum signal_result             signal_result;

	if (out_message == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	result = kernel_signal_acquire(cap, caller, CAP_WAIT, &reference);
	if (result.status != SYSCALL_STATUS_OK) return result;
	signal_result = signal_wait(reference.signal, out_message);
	kernel_signal_release(&reference);
	return signal_result_to_syscall(signal_result);
}

syscall_result_t kernel_signal_try_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message) {
	struct kernel_signal_reference reference;
	syscall_result_t               result;
	enum signal_result             signal_result;

	if (out_message == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	result = kernel_signal_acquire(cap, caller, CAP_WAIT, &reference);
	if (result.status != SYSCALL_STATUS_OK) return result;
	signal_result = signal_try_wait(reference.signal, out_message);
	kernel_signal_release(&reference);
	if (signal_result == SIGNAL_WOULD_BLOCK) return syscall_result_ok(0u);
	if (signal_result != SIGNAL_OK) return signal_result_to_syscall(signal_result);
	return syscall_result_ok(1u);
}

cap_id_t kernel_signal_grant(struct signal* target, process_id_t recipient, cap_rights_t rights) {
	struct cap_object* object = NULL;
	cap_object_id_t    object_id;
	signal_id_t        id;
	struct signal*     held;
	cap_id_t           result_cap     = CAP_ID_INVALID;
	bool               object_created = false;

	if (target == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	id   = signal_id(target);
	held = signal_acquire(id);
	if (held != target) {
		if (held != NULL) signal_release(held);
		return CAP_ID_INVALID;
	}

	object_id = signal_cap_object_id(held);
	if (object_id != CAP_OBJECT_ID_INVALID) object = cap_object_acquire(object_id);
	if (object == NULL) {
		object_id = cap_object_create_kernel((uint64_t)id, signal_handler, &object_created);
		if (object_id == CAP_OBJECT_ID_INVALID) goto out;
		if (!signal_set_cap_object_id(held, object_id)) {
			(void)cap_object_destroy_with_id(object_id);
			goto out;
		}
	}
	else {
		cap_object_release(object);
	}

	result_cap = cap_create(object_id, recipient, rights, NULL, NULL);
	if (result_cap == CAP_ID_INVALID && object_created) {
		(void)signal_destroy_cap_object(held);
	}
out:
	signal_release(held);
	return result_cap;
}

cap_id_t kernel_signal_grant_full(struct signal* target, process_id_t recipient) {
	return kernel_signal_grant(
		target, recipient, CAP_CALL | CAP_SIGNAL | CAP_READ | CAP_WAIT | CAP_MAP | CAP_DESTROY | CAP_DELEGATE);
}
