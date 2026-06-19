#include <base/channel.h>
#include <core/address_transfer.h>
#include <core/channel.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>

static syscall_result_t syscall_channel_result_to_syscall(enum channel_result result) {
	switch (result) {
	case CHANNEL_OK:
		return syscall_result_ok(0u);
	case CHANNEL_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NOT_OWNER:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NOT_FOUND:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	case CHANNEL_NO_MESSAGE:
		return syscall_result_ok(0u);
	case CHANNEL_BUFFER_TOO_SMALL:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 2u);
	case CHANNEL_QUEUE_FULL:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	case CHANNEL_NO_MEMORY:
	case CHANNEL_LIMIT_REACHED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

/* arg0 = (out) channel_id_t* out_id */
syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                        uintptr_t arg5) {
	struct process*       process;
	struct channel*       ch;
	process_id_t          owner_pid;
	channel_id_t          id;
	struct address_space* space;
	syscall_result_t      copy_result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	owner_pid = process_pid(process);
	ch        = channel_create(owner_pid);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, CHANNEL_NO_MEMORY);

	if (!process_channel_state_add(&process->channel_state, ch)) {
		channel_destroy(ch, owner_pid);
		return syscall_result_error(SYSCALL_STATUS_FAILED, CHANNEL_LIMIT_REACHED);
	}

	id          = ch->id;
	space       = syscall_current_user_space();
	copy_result = syscall_write_uintptr_arg(space, arg0, 0u, (uintptr_t)id);
	if (copy_result.status != SYSCALL_STATUS_OK) {
		process_channel_state_remove(&process->channel_state, ch);
		channel_destroy(ch, owner_pid);
		return copy_result;
	}

	return syscall_result_ok(0u);
}

/* arg0 = channel_id_t channel_id */
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5) {
	struct process*     process;
	struct channel*     ch;
	process_id_t        caller_pid;
	enum channel_result result;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	ch = channel_lookup((channel_id_t)arg0);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	caller_pid = process_pid(process);
	result     = channel_destroy(ch, caller_pid);
	if (result == CHANNEL_OK) {
		process_channel_state_remove(&process->channel_state, ch);
	}
	return syscall_channel_result_to_syscall(result);
}
