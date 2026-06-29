#include <core/sched.h>
#include <core/syscall.h>
#include <core/uthread.h>

syscall_result_t syscall_exit_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (uthread_current() == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	sched_exit_current((thread_exit_code_t)arg0);
}
