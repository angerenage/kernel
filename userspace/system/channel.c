#include <runtime/diagnostic.h>
#include <stddef.h>
#include <system/channel.h>

#include "syscall.h"

syscall_status_t channel_create(channel_id_t* out_id) {
	syscall_result_t result = syscall(SYSCALL_CHANNEL_CREATE, (uintptr_t)out_id, 0u, 0u, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_id);
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CHANNEL_CREATE, result);
	}
#endif

	return result.status;
}

syscall_status_t channel_destroy(channel_id_t channel_id) {
	syscall_result_t result = syscall(SYSCALL_CHANNEL_DESTROY, (uintptr_t)channel_id, 0u, 0u, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(channel_id);
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CHANNEL_DESTROY, result);
	}
#endif

	return result.status;
}

syscall_status_t channel_recv(channel_id_t endpoint_id, struct cap_request* out_request, void* request_buffer,
                              size_t request_buffer_size, bool* out_received) {
	syscall_result_t result;

	if (out_request == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_request);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (request_buffer_size != 0u && request_buffer == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request_buffer);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_received != NULL) *out_received = false;

	result = syscall(SYSCALL_CAP_RECV,
	                 (uintptr_t)endpoint_id,
	                 (uintptr_t)out_request,
	                 (uintptr_t)request_buffer,
	                 (uintptr_t)request_buffer_size,
	                 0u,
	                 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(endpoint_id);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_request);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request_buffer);
			break;
		case 3u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request_buffer_size);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_RECV, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_RECV, result);
	}
#endif

	if (result.status == SYSCALL_STATUS_OK && out_received != NULL) *out_received = result.value != 0u;
	return result.status;
}

syscall_status_t channel_reply(cap_call_id_t call_id, const void* response, size_t response_size,
                               syscall_status_t status) {
	syscall_result_t result = syscall(SYSCALL_CAP_REPLY,
	                                  (uintptr_t)call_id,
	                                  (uintptr_t)response,
	                                  (uintptr_t)response_size,
	                                  (uintptr_t)status,
	                                  0u,
	                                  0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(call_id);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(response);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(response_size);
			break;
		case 3u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(status);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_REPLY, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_REPLY, result);
	}
#endif

	return result.status;
}
