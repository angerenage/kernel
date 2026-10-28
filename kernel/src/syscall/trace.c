#include "trace.h"

#include <base/trace.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <kernel/trace.h>
#include <stdint.h>
#include <string.h>

syscall_result_t syscall_trace_register(uintptr_t name, uintptr_t name_length, uintptr_t arg2, uintptr_t arg3,
                                        uintptr_t arg4, uintptr_t arg5) {
	char             buffer[TRACE_LABEL_NAME_MAX + 1u] = {0};
	trace_label_id_t label;
	syscall_result_t result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (name == 0u || name_length == 0u || name_length > TRACE_LABEL_NAME_MAX) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, name == 0u ? 0u : 1u);
	}
	if (!kernel_trace_is_active()) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	result = syscall_copy_from_user(syscall_current_user_space(), name, buffer, (size_t)name_length, 0u);
	if (result.status != SYSCALL_STATUS_OK) return result;
	if (memchr(buffer, '\0', (size_t)name_length) != NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	}
	label = kernel_trace_register_string(buffer, (size_t)name_length);
	if (label == TRACE_LABEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok((uintptr_t)label);
}

syscall_result_t syscall_trace_event(uintptr_t type, uintptr_t label, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (type > TRACE_EVENT_END || label == TRACE_LABEL_ID_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, type > TRACE_EVENT_END ? 0u : 1u);
	}
	if (!kernel_trace_is_active()) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	kernel_trace_emit(KERNEL_TRACE_EVENT_USERSPACE,
	                  (enum kernel_trace_record_type)type,
	                  kernel_trace_thread_id(sched_current_thread()),
	                  (uint64_t)label);
	return syscall_result_ok(0u);
}
