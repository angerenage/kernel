#include <runtime/diagnostic.h>
#include <stddef.h>
#include <system/capability.h>
#include <system/signal.h>

#include "syscall.h"

syscall_status_t signal_create(cap_id_t* out_cap) {
	syscall_result_t result;

	if (out_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_SIGNAL_CREATE, 0u, 0u, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_CREATE, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if ((cap_id_t)result.value == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_FAILED(SYSCALL_SIGNAL_CREATE);
		return SYSCALL_STATUS_FAILED;
	}
	*out_cap = (cap_id_t)result.value;
	return SYSCALL_STATUS_OK;
}

syscall_status_t signal_send(cap_id_t cap, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint32_t flags,
                             struct signal_send_response* out_response) {
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((flags & ~((uint32_t)SIGNAL_SEND_FLAG_COALESCE)) != 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(flags);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((flags & (uint32_t)SIGNAL_SEND_FLAG_COALESCE) != 0u) {
		result = syscall(SYSCALL_SIGNAL_SEND_COALESCED,
		                 (uintptr_t)cap,
		                 (uintptr_t)arg0,
		                 (uintptr_t)arg1,
		                 (uintptr_t)arg2,
		                 (uintptr_t)arg3,
		                 (uintptr_t)out_response);
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_SEND_COALESCED, result);
	}
	else {
		result = syscall(SYSCALL_SIGNAL_SEND,
		                 (uintptr_t)cap,
		                 (uintptr_t)arg0,
		                 (uintptr_t)arg1,
		                 (uintptr_t)arg2,
		                 (uintptr_t)arg3,
		                 (uintptr_t)out_response);
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_SEND, result);
	}
	return result.status;
}

syscall_status_t signal_read(cap_id_t cap, struct signal_message* out_message, bool* out_has_value) {
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_message == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_message);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_has_value == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_has_value);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_SIGNAL_READ, (uintptr_t)cap, (uintptr_t)out_message, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_READ, result);
	if (result.status == SYSCALL_STATUS_OK) *out_has_value = result.value != 0u;
	return result.status;
}

syscall_status_t signal_read_info(cap_id_t cap, struct signal_read_response* out_response) {
	const struct signal_read_request request = {
		.header = {.op = SIGNAL_OP_READ},
	};
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_response == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_response);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(cap, &request, sizeof(request), out_response, sizeof(*out_response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(SIGNAL_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_response)) {
		RUNTIME_DIAGNOSTIC_FAILED(SIGNAL_OP_READ);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t signal_wait(cap_id_t cap, struct signal_message* out_message) {
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_message == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_message);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_SIGNAL_WAIT, (uintptr_t)cap, (uintptr_t)out_message, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_WAIT, result);
	return result.status;
}

syscall_status_t signal_try_wait(cap_id_t cap, struct signal_message* out_message, bool* out_received) {
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_message == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_message);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_received == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_received);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_SIGNAL_TRY_WAIT, (uintptr_t)cap, (uintptr_t)out_message, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_SIGNAL_TRY_WAIT, result);
	if (result.status == SYSCALL_STATUS_OK) *out_received = result.value != 0u;
	return result.status;
}

syscall_status_t signal_set_handler(cap_id_t cap, user_upcall_entry_t* handler, uint32_t flags) {
	const struct signal_set_handler_request request = {
		.header  = {.op = SIGNAL_OP_SET_HANDLER},
		.handler = handler,
		.flags   = flags,
	};
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (handler == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(handler);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if ((flags & ~(uint32_t)SIGNAL_HANDLER_FLAG_ONESHOT) != 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(flags);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(SIGNAL_OP_SET_HANDLER, result);
	return result.status;
}

syscall_status_t signal_clear_handler(cap_id_t cap) {
	const struct signal_clear_handler_request request = {
		.header = {.op = SIGNAL_OP_CLEAR_HANDLER},
	};
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(SIGNAL_OP_CLEAR_HANDLER, result);
	return result.status;
}

syscall_status_t signal_unsubscribe(cap_id_t cap) {
	const struct signal_unsubscribe_request request = {.header = {.op = SIGNAL_OP_UNSUBSCRIBE}};
	syscall_result_t                        result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(SIGNAL_OP_UNSUBSCRIBE, result);
	return result.status;
}

syscall_status_t signal_destroy(cap_id_t cap) {
	const struct signal_destroy_request request = {.header = {.op = SIGNAL_OP_DESTROY}};
	syscall_result_t                    result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(cap, &request, sizeof(request), NULL, 0u);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(SIGNAL_OP_DESTROY, result);
	return result.status;
}
