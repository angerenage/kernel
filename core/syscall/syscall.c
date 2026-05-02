#include <base/time.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <hal/clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static syscall_result_t syscall_nop(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	printf("syscall: nop called with args %p %p %p %p %p %p\n",
	       (void*)arg0,
	       (void*)arg1,
	       (void*)arg2,
	       (void*)arg3,
	       (void*)arg4,
	       (void*)arg5);

	return syscall_result_ok(0u);
}

static syscall_result_t syscall_yield(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	sched_yield();
	return syscall_result_ok(0u);
}

static syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5) {
	uint64_t deadline_tick;
	uint32_t frequency;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg0 == 0u) {
		sched_yield();
		return syscall_result_ok(0u);
	}

	frequency = hal_clock_frequency();
	if (frequency == 0u) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	if (!time_tick_deadline_from_ms(sched_tick_count(), (uint64_t)arg0, frequency, &deadline_tick)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (!sched_sleep_until_tick(deadline_tick)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	return syscall_result_ok(0u);
}

static syscall_result_t syscall_tick_count(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                           uintptr_t arg4, uintptr_t arg5) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	return syscall_result_ok((uintptr_t)sched_tick_count());
}

static syscall_result_t syscall_getpid(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
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

static syscall_result_t syscall_get_process_thread_count(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
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

static syscall_result_t syscall_gettid(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
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

static syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                               uintptr_t arg4, uintptr_t arg5) {
	struct process*     process = NULL;
	enum process_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	result = process_create(&process, (const char*)arg0);
	if (result != PROCESS_OK) return syscall_result_from_process_create(result);

	return syscall_result_ok((uintptr_t)process_pid(process));
}

static syscall_result_t syscall_run_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_exit_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                             uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_wait_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                             uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_detach_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                               uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_kill_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                             uintptr_t arg4, uintptr_t arg5) {
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

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP]                      = syscall_nop,
	[SYSCALL_YIELD]                    = syscall_yield,
	[SYSCALL_SLEEP_MS]                 = syscall_sleep_ms,
	[SYSCALL_TICK_COUNT]               = syscall_tick_count,
	[SYSCALL_GETPID]                   = syscall_getpid,
	[SYSCALL_GET_PROCESS_THREAD_COUNT] = syscall_get_process_thread_count,
	[SYSCALL_GETTID]                   = syscall_gettid,
	[SYSCALL_EXIT_PROCESS]             = syscall_exit_process,
	[SYSCALL_CREATE_PROCESS]           = syscall_create_process,
	[SYSCALL_RUN_PROCESS]              = syscall_run_process,
	[SYSCALL_WAIT_PROCESS]             = syscall_wait_process,
	[SYSCALL_DETACH_PROCESS]           = syscall_detach_process,
	[SYSCALL_KILL_PROCESS]             = syscall_kill_process,
};

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNKNOWN_SYSCALL, number);
	}
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
