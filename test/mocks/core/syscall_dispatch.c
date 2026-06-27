#include <core/syscall.h>

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP] = syscall_nop,

	[SYSCALL_YIELD]      = syscall_yield,
	[SYSCALL_SLEEP_MS]   = syscall_sleep_ms,
	[SYSCALL_TICK_COUNT] = syscall_tick_count,

	[SYSCALL_EXIT_PROCESS] = syscall_exit_process,

	[SYSCALL_EXIT_THREAD]               = syscall_exit_thread,
	[SYSCALL_SPAWN_THREAD]              = syscall_spawn_thread,
	[SYSCALL_JOIN_THREAD]               = syscall_join_thread,
	[SYSCALL_DETACH_THREAD]             = syscall_detach_thread,
	[SYSCALL_CANCEL_THREAD]             = syscall_cancel_thread,
	[SYSCALL_SET_THREAD_CANCEL_ENABLED] = syscall_set_thread_cancel_enabled,
	[SYSCALL_TEST_THREAD_CANCEL]        = syscall_test_thread_cancel,

	[SYSCALL_SEND_MESSAGE] = syscall_send_message,
	[SYSCALL_RECV_MESSAGE] = syscall_recv_message,

	[SYSCALL_CHANNEL_CREATE]  = syscall_channel_create,
	[SYSCALL_CHANNEL_DESTROY] = syscall_channel_destroy,

	[SYSCALL_CAP_CREATE]   = syscall_cap_create,
	[SYSCALL_CAP_DELEGATE] = syscall_cap_delegate,
	[SYSCALL_CAP_DERIVE]   = syscall_cap_derive,
	[SYSCALL_CAP_CALL]     = syscall_cap_call,
	[SYSCALL_CAP_REPLY]    = syscall_cap_reply,
	[SYSCALL_CAP_REVOKE]   = syscall_cap_revoke,
	[SYSCALL_CAP_RECV]     = syscall_cap_recv,
};

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNKNOWN_SYSCALL, number);
	}
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
