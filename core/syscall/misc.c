#include <base/display.h>
#include <core/address_transfer.h>
#include <core/process.h>
#include <libc/stdlib.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#include "syscall_private.h"

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

syscall_result_t syscall_nop(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                             uintptr_t arg5) {
	printf("syscall: nop called with args %p %p %p %p %p %p\n",
	       (void*)arg0,
	       (void*)arg1,
	       (void*)arg2,
	       (void*)arg3,
	       (void*)arg4,
	       (void*)arg5);

	return syscall_result_ok(0u);
}

syscall_result_t syscall_print(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                               uintptr_t arg5) {
	struct process*              process;
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	size_t                       length;
	size_t                       written = 0u;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	length = (size_t)arg1;
	if ((uintptr_t)length != arg1) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (length > (size_t)INT_MAX) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	if (length == 0u) return syscall_result_ok(0u);
	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	space = process_address_space(process);
	if (space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	transfer_result = address_space_validate_range(
		space, arg0, length, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 0u);

	{
		uint8_t* buffer = malloc(256);
		if (buffer == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

		while (written < length) {
			size_t chunk = length - written;

			if (chunk > 256u) chunk = 256u;
			transfer_result = address_space_copy_from(space, arg0 + written, buffer, chunk);
			if (transfer_result != ADDRESS_TRANSFER_OK) {
				free(buffer);
				return syscall_result_from_address_transfer(transfer_result, 0u);
			}
			base_display_write((const char*)buffer, chunk);
			written += chunk;
		}
		free(buffer);
	}
	return syscall_result_ok((uintptr_t)length);
}
