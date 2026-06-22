#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>

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
