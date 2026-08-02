#include "signal.h"

#include <base/cap.h>
#include <base/signal.h>
#include <core/address_transfer.h>
#include <core/process.h>
#include <core/signal.h>
#include <core/syscall.h>

#include "../capability/signal.h"

_Static_assert(sizeof(uintptr_t) >= sizeof(uint64_t), "signal syscall payload requires 64-bit arguments");

static syscall_result_t signal_transfer_result(enum address_transfer_result result, uintptr_t arg_index) {
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

static syscall_result_t signal_validate_write_buffer(uintptr_t address, size_t size, uintptr_t arg_index) {
	struct address_space* space;

	if (address == 0u || size == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);
	space = syscall_current_user_space();
	if (space == NULL) return syscall_result_ok(0u);
	return signal_transfer_result(
		address_space_validate_range(
			space, address, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN),
		arg_index);
}

syscall_result_t syscall_signal_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	struct process* process;
	struct signal*  signal;
	cap_id_t        cap;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	signal = signal_create();
	if (signal == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, SIGNAL_NO_MEMORY);
	cap = kernel_signal_grant_full(signal, process_pid(process));
	if (cap == CAP_ID_INVALID) {
		(void)signal_destroy(signal);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	return syscall_result_ok((uintptr_t)cap);
}

syscall_result_t syscall_signal_send(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct process*             process;
	const struct signal_payload payload = {
		.args = {(uint64_t)arg1, (uint64_t)arg2, (uint64_t)arg3, (uint64_t)arg4},
	};
	struct signal_send_response response;
	syscall_result_t            result;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg5 != 0u) {
		result = signal_validate_write_buffer(arg5, sizeof(response), 5u);
		if (result.status != SYSCALL_STATUS_OK) return result;
	}

	result = kernel_signal_send((cap_id_t)arg0, process_pid(process), &payload, arg5 != 0u ? &response : NULL);
	if (result.status != SYSCALL_STATUS_OK || arg5 == 0u) return result;
	return syscall_copy_to_user(syscall_current_user_space(), arg5, &response, sizeof(response), 5u);
}

syscall_result_t syscall_signal_read(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct process*       process;
	struct signal_message message;
	syscall_result_t      result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = signal_validate_write_buffer(arg1, sizeof(message), 1u);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = kernel_signal_read((cap_id_t)arg0, process_pid(process), &message);
	if (result.status != SYSCALL_STATUS_OK || result.value == 0u) return result;
	result = syscall_copy_to_user(syscall_current_user_space(), arg1, &message, sizeof(message), 1u);
	if (result.status == SYSCALL_STATUS_OK) result.value = 1u;
	return result;
}

syscall_result_t syscall_signal_wait(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct process*       process;
	struct signal_message message;
	syscall_result_t      result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = signal_validate_write_buffer(arg1, sizeof(message), 1u);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = kernel_signal_wait((cap_id_t)arg0, process_pid(process), &message);
	if (result.status != SYSCALL_STATUS_OK) return result;
	return syscall_copy_to_user(syscall_current_user_space(), arg1, &message, sizeof(message), 1u);
}

syscall_result_t syscall_signal_try_wait(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5) {
	struct process*       process;
	struct signal_message message;
	syscall_result_t      result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = signal_validate_write_buffer(arg1, sizeof(message), 1u);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = kernel_signal_try_wait((cap_id_t)arg0, process_pid(process), &message);
	if (result.status != SYSCALL_STATUS_OK || result.value == 0u) return result;
	result = syscall_copy_to_user(syscall_current_user_space(), arg1, &message, sizeof(message), 1u);
	if (result.status == SYSCALL_STATUS_OK) result.value = 1u;
	return result;
}
