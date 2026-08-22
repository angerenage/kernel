#include <base/channel.h>
#include <core/address_transfer.h>
#include <core/channel.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/vm_space.h>
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

syscall_result_t syscall_channel_event_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4, uintptr_t arg5) {
	struct process*              process;
	struct channel*              channel;
	struct address_space*        space;
	struct channel_event         event = {0};
	enum address_transfer_result transfer_result;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	process = process_current();
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (arg0 == CHANNEL_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (arg1 == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 1u);
	space = syscall_current_user_space();
	if (space != NULL) {
		transfer_result = address_space_validate_range(
			space, arg1, sizeof(event), ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
		if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_from_address_transfer(transfer_result, 1u);
	}

	channel = channel_acquire((channel_id_t)arg0);
	if (channel == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (channel->owner_pid != process_pid(process)) {
		channel_release(channel);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	for (;;) {
		struct cap_object* object = channel_dequeue_cap_event(channel);
		if (object == NULL) {
			channel_release(channel);
			return syscall_result_ok(0u);
		}
		if (!cap_object_consume_zero_grants_event(object, &event.object_id)) continue;
		event.type = CHANNEL_EVENT_CAP_ZERO_GRANTS;
		break;
	}
	channel_release(channel);
	syscall_result_t copy_result = syscall_copy_to_user(space, arg1, &event, sizeof(event), 1u);
	return copy_result.status == SYSCALL_STATUS_OK ? syscall_result_ok(1u) : copy_result;
}

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

	ch = channel_acquire((channel_id_t)arg0);
	if (ch == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	caller_pid = process_pid(process);
	result     = channel_destroy(ch, caller_pid);
	if (result == CHANNEL_OK) {
		process_channel_state_remove(&process->channel_state, ch);
	}
	channel_release(ch);
	return syscall_channel_result_to_syscall(result);
}
