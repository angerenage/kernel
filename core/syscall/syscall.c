#include <base/time.h>
#include <core/address_transfer.h>
#include <core/kheap.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <hal/clock.h>
#include <libk/string.h>
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

static syscall_result_t syscall_result_from_address_transfer(enum address_transfer_result result, uintptr_t arg_index) {
	switch (result) {
	case ADDRESS_TRANSFER_OK:
		return syscall_result_ok(0u);
	case ADDRESS_TRANSFER_FAULT_FAILED:
		return syscall_result_error(SYSCALL_STATUS_FAILED, arg_index);
	case ADDRESS_TRANSFER_INVALID_ARGUMENTS:
	case ADDRESS_TRANSFER_ADDRESS_OVERFLOW:
	case ADDRESS_TRANSFER_NOT_MAPPED:
	case ADDRESS_TRANSFER_NOT_USER:
	case ADDRESS_TRANSFER_ACCESS_DENIED:
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, arg_index);
	}
}

static struct address_space* syscall_current_user_space(void) {
	struct thread* current = sched_current_thread();

	if (current == NULL || current->address_space == NULL || current->address_space == address_space_kernel()) {
		return NULL;
	}
	if (!address_space_is_initialized(current->address_space)) return NULL;
	return current->address_space;
}

static syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                                uintptr_t string_len_arg, char** out_string) {
	struct address_space*        space;
	enum address_transfer_result transfer_result;
	size_t                       string_len = (size_t)string_len_arg;
	char*                        string;

	if (out_string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, ptr_arg_index);
	*out_string = NULL;
	if (string_ptr == 0u && string_len_arg == 0u) return syscall_result_ok(0u);
	if (string_ptr == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, ptr_arg_index);
	if (string_len_arg == 0u || (uintptr_t)string_len != string_len_arg) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
	}

	space = syscall_current_user_space();
	if (space != NULL) {
		transfer_result = address_space_validate_range(
			space, string_ptr, string_len, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			return syscall_result_from_address_transfer(transfer_result, ptr_arg_index);
		}
	}

	if (space == NULL) {
		if (((const char*)string_ptr)[string_len - 1u] != '\0') {
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
		}
		string = strndup((const char*)string_ptr, string_len - 1u);
		if (string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, len_arg_index);
	}
	else {
		string = kmalloc(string_len);
		if (string == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, len_arg_index);

		transfer_result = address_space_copy_from(space, string_ptr, string, string_len);
		if (transfer_result != ADDRESS_TRANSFER_OK) {
			kfree(string);
			return syscall_result_from_address_transfer(transfer_result, ptr_arg_index);
		}
		if (string[string_len - 1u] != '\0') {
			kfree(string);
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, len_arg_index);
		}
	}

	*out_string = string;
	return syscall_result_ok(0u);
}

static syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                               uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_exit_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_spawn_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                             uintptr_t arg4, uintptr_t arg5) {
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
	kfree(name);
	if (result != PROCESS_THREAD_SPAWN_OK) return syscall_result_from_thread_spawn(result);

	return syscall_result_ok(detached ? 0u : (uintptr_t)uthread_id(thread));
}

static syscall_result_t syscall_join_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_detach_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                              uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_cancel_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                              uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_set_thread_cancel_enabled(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2,
                                                          uintptr_t arg3, uintptr_t arg4, uintptr_t arg5) {
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

static syscall_result_t syscall_test_thread_cancel(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
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

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP] = syscall_nop,

	[SYSCALL_YIELD]      = syscall_yield,
	[SYSCALL_SLEEP_MS]   = syscall_sleep_ms,
	[SYSCALL_TICK_COUNT] = syscall_tick_count,

	[SYSCALL_GETPID]                   = syscall_getpid,
	[SYSCALL_GET_PROCESS_THREAD_COUNT] = syscall_get_process_thread_count,

	[SYSCALL_GETTID] = syscall_gettid,

	[SYSCALL_EXIT_PROCESS]   = syscall_exit_process,
	[SYSCALL_CREATE_PROCESS] = syscall_create_process,
	[SYSCALL_RUN_PROCESS]    = syscall_run_process,
	[SYSCALL_WAIT_PROCESS]   = syscall_wait_process,
	[SYSCALL_DETACH_PROCESS] = syscall_detach_process,
	[SYSCALL_KILL_PROCESS]   = syscall_kill_process,

	[SYSCALL_EXIT_THREAD]               = syscall_exit_thread,
	[SYSCALL_SPAWN_THREAD]              = syscall_spawn_thread,
	[SYSCALL_JOIN_THREAD]               = syscall_join_thread,
	[SYSCALL_DETACH_THREAD]             = syscall_detach_thread,
	[SYSCALL_CANCEL_THREAD]             = syscall_cancel_thread,
	[SYSCALL_SET_THREAD_CANCEL_ENABLED] = syscall_set_thread_cancel_enabled,
	[SYSCALL_TEST_THREAD_CANCEL]        = syscall_test_thread_cancel,
};

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNKNOWN_SYSCALL, number);
	}
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
