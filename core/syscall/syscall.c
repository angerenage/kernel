#include <core/address_transfer.h>
#include <core/sched.h>
#include <core/vaddr_alloc.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <string.h>

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

static struct address_space* syscall_current_user_space(void) {
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

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP] = syscall_nop,

	[SYSCALL_PRINT] = syscall_print,

	[SYSCALL_YIELD]      = syscall_yield,
	[SYSCALL_SLEEP_MS]   = syscall_sleep_ms,
	[SYSCALL_TICK_COUNT] = syscall_tick_count,

	[SYSCALL_GETPID]                   = syscall_getpid,
	[SYSCALL_GET_PROCESS_THREAD_COUNT] = syscall_get_process_thread_count,

	[SYSCALL_GETTID] = syscall_gettid,

	[SYSCALL_EXIT_PROCESS]   = syscall_exit_process,
	[SYSCALL_CREATE_PROCESS] = syscall_create_process,
	[SYSCALL_RUN_PROCESS]    = syscall_run_process,
	[SYSCALL_WAIT_PROCESS]   = syscall_wait_process,
	[SYSCALL_DETACH_PROCESS] = syscall_detach_process,
	[SYSCALL_KILL_PROCESS]   = syscall_kill_process,

	[SYSCALL_EXIT_THREAD]               = syscall_exit_thread,
	[SYSCALL_SPAWN_THREAD]              = syscall_spawn_thread,
	[SYSCALL_JOIN_THREAD]               = syscall_join_thread,
	[SYSCALL_DETACH_THREAD]             = syscall_detach_thread,
	[SYSCALL_CANCEL_THREAD]             = syscall_cancel_thread,
	[SYSCALL_SET_THREAD_CANCEL_ENABLED] = syscall_set_thread_cancel_enabled,
	[SYSCALL_TEST_THREAD_CANCEL]        = syscall_test_thread_cancel,

	[SYSCALL_VM_RESERVE]    = syscall_vm_reserve,
	[SYSCALL_VM_RESERVE_AT] = syscall_vm_reserve_at,
	[SYSCALL_VM_FREE]       = syscall_vm_free,
	[SYSCALL_VM_MAP]        = syscall_vm_map,
	[SYSCALL_VM_UNMAP]      = syscall_vm_unmap,
	[SYSCALL_VM_PROTECT]    = syscall_vm_protect,
	[SYSCALL_VM_QUERY]      = syscall_vm_query,
	[SYSCALL_VM_COPY_FROM]  = syscall_vm_copy_from,
	[SYSCALL_VM_COPY_TO]    = syscall_vm_copy_to,

	[SYSCALL_SEND_MESSAGE] = syscall_send_message,
	[SYSCALL_RECV_MESSAGE] = syscall_recv_message,
};

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNKNOWN_SYSCALL, number);
	}
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
