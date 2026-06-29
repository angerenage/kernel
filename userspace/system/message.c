#include <stdbool.h>
#include <stddef.h>
#include <system/message.h>

#include "syscall.h"

bool message_send(process_id_t pid, const void* buffer, size_t length) {
	syscall_result_t result =
		syscall(SYSCALL_SEND_MESSAGE, (uintptr_t)pid, (uintptr_t)buffer, (uintptr_t)length, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

message_recv_status_t message_recv(void* buffer, size_t buffer_size, size_t* out_length, process_id_t* out_sender_pid,
                                   size_t* out_required_size) {
	uintptr_t        length_value = 0u;
	uintptr_t        sender_value = 0u;
	syscall_result_t result       = syscall(SYSCALL_RECV_MESSAGE,
                                      (uintptr_t)buffer,
                                      (uintptr_t)&length_value,
                                      (uintptr_t)buffer_size,
                                      (uintptr_t)&sender_value,
                                      0u,
                                      0u);

	if (result.status != SYSCALL_STATUS_OK) {
		if (result.status == SYSCALL_STATUS_BAD_ARGUMENT && result.value == 2u) {
			if (out_required_size != NULL) *out_required_size = (size_t)length_value;
			return MESSAGE_RECV_TOO_SMALL;
		}
		return MESSAGE_RECV_FAILED;
	}
	if (result.value == 0u) return MESSAGE_RECV_EMPTY;
	if (out_length != NULL) *out_length = (size_t)length_value;
	if (out_sender_pid != NULL) *out_sender_pid = (process_id_t)sender_value;
	return MESSAGE_RECV_OK;
}
