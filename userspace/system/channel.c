#include <stddef.h>
#include <system/channel.h>

#include "syscall.h"

syscall_status_t channel_create(channel_id_t* out_id) {
	syscall_result_t result = syscall(SYSCALL_CHANNEL_CREATE, (uintptr_t)out_id, 0u, 0u, 0u, 0u, 0u);
	return result.status;
}

syscall_status_t channel_destroy(channel_id_t channel_id) {
	syscall_result_t result = syscall(SYSCALL_CHANNEL_DESTROY, (uintptr_t)channel_id, 0u, 0u, 0u, 0u, 0u);
	return result.status;
}

syscall_status_t channel_recv(channel_id_t endpoint_id, struct cap_request* out_request, void* request_buffer,
                              size_t request_buffer_size) {
	syscall_result_t result;

	if (out_request == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (request_buffer_size != 0u && request_buffer == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	result = syscall(SYSCALL_CAP_RECV,
	                 (uintptr_t)endpoint_id,
	                 (uintptr_t)out_request,
	                 (uintptr_t)request_buffer,
	                 (uintptr_t)request_buffer_size,
	                 0u,
	                 0u);
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

	return result.status;
}
