#include "process.h"

#include <base/cap.h>
#include <base/self.h>
#include <base/syscall.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <libc/stdlib.h>

#include "../capability/address_space.h"
#include "../capability/process.h"

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

syscall_result_t syscall_self(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                              uintptr_t arg5) {
	struct process*  process;
	struct uthread*  thread;
	struct self_info info;
	cap_id_t         self_cap_id;
	cap_id_t         address_space_cap_id;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	thread = uthread_current();

	self_cap_id = kernel_self_grant(process);
	if (self_cap_id == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	address_space_cap_id =
		kernel_address_space_grant(process, CAP_ALLOCATE | CAP_DESTROY | CAP_MAP | CAP_READ | CAP_WRITE);
	if (address_space_cap_id == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	info.pid               = process_pid(process);
	info.thread_id         = thread != NULL ? (uint64_t)uthread_id(thread) : 0u;
	info.thread_count      = process_thread_count(process);
	info.self_cap          = self_cap_id;
	info.address_space_cap = address_space_cap_id;

	return syscall_copy_to_user(process_address_space(process), arg0, &info, sizeof(info), 0u);
}

syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	struct process*     process = NULL;
	enum process_result result;
	char*               name = NULL;
	syscall_result_t    copy_result;
	cap_id_t            cap_id;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	copy_result = syscall_copy_string_arg(0u, arg0, 1u, arg1, &name);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	result = process_create(&process, name);
	free(name);
	if (result != PROCESS_OK) return syscall_result_from_process_create(result);

	cap_id = kernel_self_grant(process);
	if (cap_id == CAP_ID_INVALID) {
		process_destroy(process);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	return syscall_result_ok((uintptr_t)cap_id);
}
