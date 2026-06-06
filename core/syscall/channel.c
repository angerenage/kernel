#include <base/channel.h>
#include <base/message.h>
#include <core/address_transfer.h>
#include <core/channel.h>
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

static syscall_result_t syscall_channel_result_to_syscall(enum channel_result result) {
	switch (result) {
	case CHANNEL_OK:
		return syscall_result_ok(0u);
	case CHANNEL_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NOT_OWNER:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NOT_FOUND:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NO_MESSAGE:
		return syscall_result_ok(0u);
	case CHANNEL_BUFFER_TOO_SMALL:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	case CHANNEL_QUEUE_FULL:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	case CHANNEL_NO_MEMORY:
	case CHANNEL_LIMIT_REACHED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

/* arg0 = (out) channel_id_t* out_id */
syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	struct process*       process;
	struct channel*       ch;
	process_id_t          owner_pid;
	channel_id_t          id;
	struct address_space* space;
	syscall_result_t      copy_result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	owner_pid = process_pid(process);
	ch        = channel_create(owner_pid);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, CHANNEL_NO_MEMORY);

	if (!process_channel_state_add(&process->channel_state, ch)) {
		channel_destroy(ch, owner_pid);
		return syscall_result_error(SYSCALL_STATUS_FAILED, CHANNEL_LIMIT_REACHED);
	}

	id          = ch->id;
	space       = syscall_current_user_space();
	copy_result = syscall_write_uintptr_arg(space, arg0, 0u, (uintptr_t)id);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		process_channel_state_remove(&process->channel_state, ch);
		channel_destroy(ch, owner_pid);
		return copy_result;
	}

	return syscall_result_ok(0u);
}

/* arg0 = channel_id_t channel_id, arg1 = const void* buffer, arg2 = size_t length */
syscall_result_t syscall_channel_send(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              process;
	struct channel*              ch;
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	enum channel_result          ch_result;
	size_t                       length;
	uint8_t                      payload[MESSAGE_MAX_SIZE];
	const void*                  source;
	process_id_t                 sender_pid;

	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	length = (size_t)arg2;
	if ((uintptr_t)length != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	if (length > MESSAGE_MAX_SIZE) return syscall_result_error(SYSCALL_STATUS_FAILED, MESSAGE_TOO_LARGE);
	if (length > 0u && arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	ch = channel_lookup((channel_id_t)arg0);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

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

	sender_pid = process_pid(process);
	ch_result  = channel_send(ch, sender_pid, source, length);
	return syscall_channel_result_to_syscall(ch_result);
}

/* arg0 = channel_id_t channel_id, arg1 = void* buffer, arg2 = size_t buffer_size,
 * arg3 = size_t* out_length, arg4 = process_id_t* out_sender_pid */
syscall_result_t syscall_channel_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*              process;
	struct channel*              ch;
	struct address_space*        space;
	enum channel_result          ch_result;
	enum address_transfer_result transfer_result;
	syscall_result_t             copy_result;
	size_t                       buffer_size;
	size_t                       length     = 0u;
	process_id_t                 sender_pid = PROCESS_PID_INVALID;
	process_id_t                 caller_pid;
	uint8_t                      payload[MESSAGE_MAX_SIZE];

	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (arg3 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 3u);
	if (arg4 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 4u);

	buffer_size = (size_t)arg2;
	if ((uintptr_t)buffer_size != arg2) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);

	ch = channel_lookup((channel_id_t)arg0);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	caller_pid = process_pid(process);
	space      = syscall_current_user_space();
	if (space != NULL && buffer_size > 0u) {
		transfer_result = address_space_validate_range(
			space, arg1, buffer_size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			return syscall_result_from_address_transfer(transfer_result, 1u);
		}
	}

	ch_result = channel_recv(ch, caller_pid, payload, buffer_size, &length, &sender_pid);
	if (ch_result == CHANNEL_NO_MESSAGE) {
		return syscall_result_ok(0u);
	}
	if (ch_result == CHANNEL_BUFFER_TOO_SMALL) {
		copy_result = syscall_write_uintptr_arg(space, arg3, 3u, (uintptr_t)length);
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	}
	if (ch_result != CHANNEL_OK) {
		return syscall_channel_result_to_syscall(ch_result);
	}

	copy_result = syscall_copy_to_user(space, arg1, payload, length, 1u);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		return copy_result;
	}
	copy_result = syscall_write_uintptr_arg(space, arg3, 3u, (uintptr_t)length);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		return copy_result;
	}
	copy_result = syscall_write_uintptr_arg(space, arg4, 4u, (uintptr_t)sender_pid);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	return syscall_result_ok(1u);
}

/* arg0 = channel_id_t channel_id */
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5) {
	struct process*     process;
	struct channel*     ch;
	process_id_t        caller_pid;
	enum channel_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	ch = channel_lookup((channel_id_t)arg0);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	caller_pid = process_pid(process);
	result     = channel_destroy(ch, caller_pid);
	if (result == CHANNEL_OK) {
		process_channel_state_remove(&process->channel_state, ch);
	}
	return syscall_channel_result_to_syscall(result);
}
