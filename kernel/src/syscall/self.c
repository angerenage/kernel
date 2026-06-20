#include "self.h"

#include <base/cap.h>
#include <base/self.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>

#include "../capability/process.h"

syscall_result_t syscall_self(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                              uintptr_t arg5) {
	struct process*  process;
	struct uthread*  thread;
	struct self_info info;
	cap_id_t         cap_id;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	thread = uthread_current();

	cap_id = kernel_self_grant(process);
	if (cap_id == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	info.pid          = process_pid(process);
	info.thread_id    = thread != NULL ? (uint64_t)uthread_id(thread) : 0u;
	info.thread_count = process_thread_count(process);
	info.self_cap     = cap_id;

	return syscall_copy_to_user(process_address_space(process), arg0, &info, sizeof(info), 0u);
}
