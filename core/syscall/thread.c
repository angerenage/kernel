#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <libc/stdlib.h>

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

static syscall_result_t syscall_join_result_from_thread_join(enum process_thread_join_result result,
                                                             uintptr_t                       exit_code) {
	switch (result) {
	case PROCESS_THREAD_JOIN_OK:
		return syscall_result_ok(exit_code);
	case PROCESS_THREAD_JOIN_INVALID_ARGUMENTS:
	case PROCESS_THREAD_JOIN_FOREIGN_THREAD:
	case PROCESS_THREAD_JOIN_SELF:
	case PROCESS_THREAD_JOIN_DETACHED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_THREAD_JOIN_WAIT_FAILED:
	case PROCESS_THREAD_JOIN_RECLAIM_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t syscall_result_from_thread_detach(enum process_thread_detach_result result) {
	switch (result) {
	case PROCESS_THREAD_DETACH_OK:
		return syscall_result_ok(0u);
	case PROCESS_THREAD_DETACH_INVALID_ARGUMENTS:
	case PROCESS_THREAD_DETACH_FOREIGN_THREAD:
	case PROCESS_THREAD_DETACH_ALREADY_DETACHED:
	case PROCESS_THREAD_DETACH_ALREADY_TERMINATED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_THREAD_DETACH_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static syscall_result_t syscall_result_from_thread_cancel(enum process_thread_cancel_result result) {
	switch (result) {
	case PROCESS_THREAD_CANCEL_OK:
		return syscall_result_ok(0u);
	case PROCESS_THREAD_CANCEL_INVALID_ARGUMENTS:
	case PROCESS_THREAD_CANCEL_FOREIGN_THREAD:
	case PROCESS_THREAD_CANCEL_ALREADY_TERMINATED:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case PROCESS_THREAD_CANCEL_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

syscall_result_t syscall_gettid(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                uintptr_t arg5) {
	struct uthread* thread;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	thread = uthread_current();
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	return syscall_result_ok((uintptr_t)uthread_id(thread));
}

syscall_result_t syscall_exit_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct uthread* thread;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	thread = uthread_current();
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	sched_exit_current(arg0);
}

syscall_result_t syscall_spawn_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	struct process*                  process;
	struct uthread*                  thread = NULL;
	enum process_thread_spawn_result result;
	struct process_thread_params     params;
	bool                             detached;
	char*                            name = NULL;
	syscall_result_t                 copy_result;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	copy_result = syscall_copy_string_arg(4u, arg4, 5u, arg5, &name);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	detached                = arg3 != 0u;
	params.name             = name;
	params.user_entry       = arg0;
	params.user_arg         = arg1;
	params.user_stack_pages = (size_t)arg2;
	params.preferred_cpu    = NULL;
	params.detached         = detached;
	result                  = process_spawn_thread(process, detached ? NULL : &thread, &params);
	free(name);
	if (result != PROCESS_THREAD_SPAWN_OK) return syscall_result_from_thread_spawn(result);

	return syscall_result_ok(detached ? 0u : (uintptr_t)uthread_id(thread));
}

syscall_result_t syscall_join_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                     uintptr_t arg5) {
	struct process*                 process;
	struct uthread*                 thread;
	uintptr_t                       exit_code = 0u;
	enum process_thread_join_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	thread = uthread_lookup((uthread_id_t)arg0);
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	result = process_join_thread(process, thread, &exit_code);
	return syscall_join_result_from_thread_join(result, exit_code);
}

syscall_result_t syscall_detach_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	struct process*                   process;
	struct uthread*                   thread;
	enum process_thread_detach_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	thread = uthread_lookup((uthread_id_t)arg0);
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	result = process_detach_thread(process, thread);
	return syscall_result_from_thread_detach(result);
}

syscall_result_t syscall_cancel_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5) {
	struct process*                   process;
	struct uthread*                   thread;
	enum process_thread_cancel_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	thread = uthread_lookup((uthread_id_t)arg0);
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	result = process_cancel_thread(process, thread);
	return syscall_result_from_thread_cancel(result);
}

syscall_result_t syscall_set_thread_cancel_enabled(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                                   uintptr_t arg4, uintptr_t arg5) {
	struct uthread* thread;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	thread = uthread_current();
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	thread_set_cancel_enabled(&thread->thread, arg0 != 0u);
	return syscall_result_ok(0u);
}

syscall_result_t syscall_test_thread_cancel(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4, uintptr_t arg5) {
	struct uthread* thread;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	thread = uthread_current();
	if (thread == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (thread_should_cancel(&thread->thread)) sched_exit_current(THREAD_EXIT_CODE_CANCELLED);
	return syscall_result_ok(0u);
}
