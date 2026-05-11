#include <base/message.h>
#include <core/address_transfer.h>
#include <core/message.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <string.h>

#include "syscall_private.h"

static struct address_space* syscall_current_user_space(void) {
	struct thread* current = sched_current_thread();

	if (current == NULL || current->address_space == NULL || current->address_space == address_space_kernel()) {
		return NULL;
	}
	if (!address_space_is_initialized(current->address_space)) return NULL;
	return current->address_space;
}

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

static syscall_result_t syscall_write_uintptr_arg(struct address_space* space, uintptr_t dst, uintptr_t arg_index,
                                                  uintptr_t value) {
	enum address_transfer_result transfer_result;

	if (dst == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);

	if (space == NULL) {
		*(uintptr_t*)dst = value;
		return syscall_result_ok(0u);
	}

	transfer_result = address_space_write_uintptr(space, dst, value);
	return syscall_result_from_address_transfer(transfer_result, arg_index);
}

static syscall_result_t syscall_copy_to_user(struct address_space* space, uintptr_t dst, const void* src, size_t size,
                                             uintptr_t arg_index) {
	enum address_transfer_result transfer_result;

	if (size == 0u) return syscall_result_ok(0u);
	if (dst == 0u || src == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);

	if (space == NULL) {
		memcpy((void*)dst, src, size);
		return syscall_result_ok(0u);
	}

	transfer_result = address_space_copy_to(space, dst, src, size);
	return syscall_result_from_address_transfer(transfer_result, arg_index);
}

syscall_result_t syscall_send_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              target;
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	size_t                       length;
	uint8_t                      payload[MESSAGE_MAX_SIZE];
	const void*                  source;
	enum message_result          result;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	length = (size_t)arg2;
	if ((uintptr_t)length != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (length > MESSAGE_MAX_SIZE) return syscall_result_error(SYSCALL_STATUS_FAILED, MESSAGE_TOO_LARGE);
	if (length > 0u && arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	target = process_lookup((process_id_t)arg0);
	if (target == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, MESSAGE_INVALID_PID);

	space = syscall_current_user_space();
	if (length == 0u) {
		source = NULL;
	}
	else if (space == NULL) {
		source = (const void*)arg1;
	}
	else {
		transfer_result = address_space_copy_from(space, arg1, payload, length);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			return syscall_result_from_address_transfer(transfer_result, 1u);
		}
		source = payload;
	}

	result = message_queue_send(&target->message_queue, source, length);
	if (result == MESSAGE_OK) return syscall_result_ok(0u);
	if (result == MESSAGE_INVALID_ARGUMENTS) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
}

syscall_result_t syscall_recv_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              process;
	struct address_space*        space;
	size_t                       length = 0u;
	enum message_result          result;
	enum address_transfer_result transfer_result;
	syscall_result_t             copy_result;
	uint8_t                      payload[MESSAGE_MAX_SIZE];

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	space = syscall_current_user_space();

	if (space != NULL) {
		transfer_result = address_space_validate_range(
			space, arg0, MESSAGE_MAX_SIZE, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			return syscall_result_from_address_transfer(transfer_result, 0u);
		}
	}

	result = message_queue_receive(&process->message_queue, payload, &length);
	if (result == MESSAGE_NO_MESSAGE) return syscall_result_ok(0u);
	if (result == MESSAGE_INVALID_ARGUMENTS) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (result != MESSAGE_OK) return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);

	copy_result = syscall_copy_to_user(space, arg0, payload, length, 0u);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	copy_result = syscall_write_uintptr_arg(space, arg1, 1u, (uintptr_t)length);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	return syscall_result_ok(1u);
}
