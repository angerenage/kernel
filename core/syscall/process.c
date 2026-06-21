#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <libc/stdlib.h>

static syscall_result_t syscall_result_from_process_create(enum process_result result) {
	switch (result) {
	case PROCESS_OK:
		return syscall_result_ok(0u);
	case PROCESS_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_NO_MEMORY:
	case PROCESS_ADDRESS_SPACE_FAILED:
	case PROCESS_PID_EXHAUSTED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	struct process*     process = NULL;
	enum process_result result;
	char*               name = NULL;
	syscall_result_t    copy_result;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	copy_result = syscall_copy_string_arg(0u, arg0, 1u, arg1, &name);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	result = process_create(&process, name);
	free(name);
	if (result != PROCESS_OK) return syscall_result_from_process_create(result);

	return syscall_result_ok((uintptr_t)process_pid(process));
}

syscall_result_t syscall_exit_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process* process;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (!process_terminate(process, arg0)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	return syscall_result_ok(0u);
}
