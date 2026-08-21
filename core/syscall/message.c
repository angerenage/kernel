#include <base/message.h>
#include <core/address_transfer.h>
#include <core/message.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/vm_space.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <string.h>

syscall_result_t syscall_send_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              target;
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	size_t                       length;
	uint8_t*                     heap_payload = NULL;
	const void*                  source;
	enum message_result          result;
	process_id_t                 sender_pid = PROCESS_PID_INVALID;
	syscall_result_t             ret;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	length = (size_t)arg2;
	if ((uintptr_t)length != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (length > MESSAGE_MAX_SIZE) return syscall_result_error(SYSCALL_STATUS_FAILED, MESSAGE_TOO_LARGE);
	if (length > 0u && arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	heap_payload = malloc(MESSAGE_MAX_SIZE);
	if (heap_payload == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 2u);

	{
		struct process* sender = process_current();
		if (sender != NULL) sender_pid = process_pid(sender);
	}

	space = syscall_current_user_space();
	if (length == 0u) {
		source = NULL;
	}
	else if (space == NULL) {
		source = (const void*)arg1;
	}
	else {
		transfer_result = address_space_copy_from(space, arg1, heap_payload, length);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			free(heap_payload);
			return syscall_result_from_address_transfer(transfer_result, 1u);
		}
		source = heap_payload;
	}

	target = process_acquire((process_id_t)arg0);
	if (target == NULL) {
		free(heap_payload);
		return syscall_result_error(SYSCALL_STATUS_FAILED, MESSAGE_INVALID_PID);
	}

	result = message_queue_send(&target->message_queue, sender_pid, source, length);
	process_release(target);
	ret = (result == MESSAGE_OK)                  ? syscall_result_ok(0u)
	      : (result == MESSAGE_INVALID_ARGUMENTS) ? syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u)
	                                              : syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	free(heap_payload);
	return ret;
}

syscall_result_t syscall_recv_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              process;
	struct address_space*        space;
	size_t                       buffer_size;
	size_t                       length     = 0u;
	process_id_t                 sender_pid = PROCESS_PID_INVALID;
	enum message_result          result;
	enum address_transfer_result transfer_result;
	syscall_result_t             copy_result;
	syscall_result_t             ret;
	uint8_t*                     heap_payload;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	buffer_size = (size_t)arg2;
	if ((uintptr_t)buffer_size != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (buffer_size > 0u && arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (arg3 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);

	heap_payload = malloc(MESSAGE_MAX_SIZE);
	if (heap_payload == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 2u);

	space = syscall_current_user_space();

	if (space != NULL && buffer_size > 0u) {
		transfer_result = address_space_validate_range(
			space, arg0, buffer_size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			free(heap_payload);
			return syscall_result_from_address_transfer(transfer_result, 0u);
		}
	}
	if (space != NULL) {
		transfer_result = address_space_validate_range(
			space, arg1, sizeof(uintptr_t), ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			free(heap_payload);
			return syscall_result_from_address_transfer(transfer_result, 1u);
		}
		transfer_result = address_space_validate_range(
			space, arg3, sizeof(uintptr_t), ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			free(heap_payload);
			return syscall_result_from_address_transfer(transfer_result, 3u);
		}
	}

	result = message_queue_receive(&process->message_queue, heap_payload, buffer_size, &length, &sender_pid);
	if (result == MESSAGE_NO_MESSAGE) {
		free(heap_payload);
		return syscall_result_ok(0u);
	}
	if (result == MESSAGE_INVALID_ARGUMENTS) {
		free(heap_payload);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (result == MESSAGE_TOO_LARGE) {
		copy_result = syscall_write_uintptr_arg(space, arg1, 1u, (uintptr_t)length);
		free(heap_payload);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}
	if (result != MESSAGE_OK) {
		free(heap_payload);
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}

	copy_result = syscall_copy_to_user(space, arg0, heap_payload, length, 0u);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		free(heap_payload);
		return copy_result;
	}

	copy_result = syscall_write_uintptr_arg(space, arg1, 1u, (uintptr_t)length);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		free(heap_payload);
		return copy_result;
	}

	copy_result = syscall_write_uintptr_arg(space, arg3, 3u, (uintptr_t)sender_pid);
	ret         = (copy_result.status == SYSCALL_STATUS_OK) ? syscall_result_ok(1u)
	                                                        : syscall_result_error(copy_result.status, copy_result.value);
	free(heap_payload);
	return ret;
}
