#include <core/kheap.h>
#include <core/process.h>
#include <core/uthread.h>

#include "syscall_private.h"

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

static syscall_result_t syscall_result_from_thread_spawn(enum process_thread_spawn_result result) {
	switch (result) {
	case PROCESS_THREAD_SPAWN_OK:
		return syscall_result_ok(0u);
	case PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_THREAD_SPAWN_NO_MEMORY:
	case PROCESS_THREAD_SPAWN_STACK_ALLOC_FAILED:
	case PROCESS_THREAD_SPAWN_CONTEXT_UNSUPPORTED:
	case PROCESS_THREAD_SPAWN_SCHEDULER_REJECTED:
	case PROCESS_THREAD_SPAWN_REAPER_UNAVAILABLE:
	case PROCESS_THREAD_SPAWN_ID_EXHAUSTED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

syscall_result_t syscall_getpid(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                uintptr_t arg5) {
	struct process* process;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok((uintptr_t)process_pid(process));
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
	kfree(name);
	if (result != PROCESS_OK) return syscall_result_from_process_create(result);

	return syscall_result_ok((uintptr_t)process_pid(process));
}

syscall_result_t syscall_run_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct process*                  process;
	struct uthread*                  main_thread = NULL;
	enum process_thread_spawn_result result;

	(void)arg4;
	(void)arg5;

	process = process_lookup((process_id_t)arg0);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);

	result = process_start_main_thread(process,
	                                   &main_thread,
	                                   &(const struct process_thread_params){
										   .name             = process->name,
										   .user_entry       = arg1,
										   .user_arg         = arg2,
										   .user_stack_pages = (size_t)arg3,
										   .preferred_cpu    = NULL,
										   .detached         = false,
									   });
	if (result != PROCESS_THREAD_SPAWN_OK) return syscall_result_from_thread_spawn(result);

	return syscall_result_ok((uintptr_t)uthread_id(main_thread));
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

syscall_result_t syscall_wait_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*          process;
	uintptr_t                exit_code = 0u;
	enum process_join_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_lookup((process_id_t)arg0);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	result = process_join(process, &exit_code);
	switch (result) {
	case PROCESS_JOIN_OK:
		if (!process_destroy(process)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
		return syscall_result_ok(exit_code);
	case PROCESS_JOIN_INVALID_ARGUMENTS:
	case PROCESS_JOIN_SELF:
	case PROCESS_JOIN_DETACHED:
	case PROCESS_JOIN_ALREADY_JOINED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_JOIN_WAIT_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

syscall_result_t syscall_detach_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	struct process*            process;
	enum process_detach_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_lookup((process_id_t)arg0);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	result = process_detach(process);
	switch (result) {
	case PROCESS_DETACH_OK:
		return syscall_result_ok(0u);
	case PROCESS_DETACH_INVALID_ARGUMENTS:
	case PROCESS_DETACH_ALREADY_DETACHED:
	case PROCESS_DETACH_ALREADY_JOINED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

syscall_result_t syscall_kill_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process* process;

	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_lookup((process_id_t)arg0);
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!process_terminate(process, arg1)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_get_process_thread_count(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                                  uintptr_t arg4, uintptr_t arg5) {
	struct process* process;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok((uintptr_t)process_thread_count(process));
}
