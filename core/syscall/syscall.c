#include <core/address_transfer.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <libc/stdlib.h>
#include <libc/string.h>

syscall_result_t syscall_result_from_address_transfer(enum address_transfer_result result, uintptr_t arg_index) {
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

struct address_space* syscall_current_user_space(void) {
	struct thread* current = sched_current_thread();

	if (current == NULL || current->address_space == NULL || current->address_space == address_space_kernel()) {
		return NULL;
	}
	if (!address_space_is_initialized(current->address_space)) return NULL;
	return current->address_space;
}

syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string) {
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	size_t                       string_len = (size_t)string_len_arg;
	char*                        string;

	if (out_string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, ptr_arg_index);
	*out_string = NULL;
	if (string_ptr == 0u && string_len_arg == 0u) return syscall_result_ok(0u);
	if (string_ptr == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, ptr_arg_index);
	if (string_len_arg == 0u || (uintptr_t)string_len != string_len_arg) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
	}

	space = syscall_current_user_space();
	if (space != NULL) {
		transfer_result = address_space_validate_range(
			space, string_ptr, string_len, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			return syscall_result_from_address_transfer(transfer_result, ptr_arg_index);
		}
	}

	if (space == NULL) {
		if (((const char*)string_ptr)[string_len - 1u] != '\0') {
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
		}
		string = strndup((const char*)string_ptr, string_len - 1u);
		if (string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, len_arg_index);
	}
	else {
		string = malloc(string_len);
		if (string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, len_arg_index);

		transfer_result = address_space_copy_from(space, string_ptr, string, string_len);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			free(string);
			return syscall_result_from_address_transfer(transfer_result, ptr_arg_index);
		}
		if (string[string_len - 1u] != '\0') {
			free(string);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
		}
	}

	*out_string = string;
	return syscall_result_ok(0u);
}

syscall_result_t syscall_write_uintptr_arg(struct address_space* space, uintptr_t dst, uintptr_t arg_index,
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

syscall_result_t syscall_copy_to_user(struct address_space* space, uintptr_t dst, const void* src, size_t size,
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

syscall_result_t syscall_copy_from_user(struct address_space* space, uintptr_t src, void* dst, size_t size,
                                        uintptr_t arg_index) {
	enum address_transfer_result transfer_result;

	if (size == 0u) return syscall_result_ok(0u);
	if (src == 0u || dst == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);

	if (space == NULL) {
		memcpy(dst, (const void*)src, size);
		return syscall_result_ok(0u);
	}

	transfer_result = address_space_copy_from(space, src, dst, size);
	return syscall_result_from_address_transfer(transfer_result, arg_index);
}
