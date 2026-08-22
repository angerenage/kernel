#include "upcall.h"

#include <core/syscall.h>
#include <core/uthread.h>

syscall_result_t syscall_upcall_dropped_count(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                              uintptr_t arg4, uintptr_t arg5) {
	struct uthread* current = uthread_current();

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;
	if (current == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok((uintptr_t)uthread_upcall_dropped_count(current));
}
