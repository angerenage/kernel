#include <kernel/syscall.h>
#include <stddef.h>

#include "capability.h"
#include "channel.h"
#include "memory.h"
#include "message.h"
#include "misc.h"
#include "module.h"
#include "process.h"
#include "signal.h"
#include "thread.h"
#include "time.h"
#include "upcall.h"

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP] = syscall_nop,

	[SYSCALL_YIELD]      = syscall_yield,
	[SYSCALL_SLEEP_MS]   = syscall_sleep_ms,
	[SYSCALL_TICK_COUNT] = syscall_tick_count,

	[SYSCALL_SELF] = syscall_self,

	[SYSCALL_EXIT_PROCESS]   = syscall_exit_process,
	[SYSCALL_EXIT_THREAD]    = syscall_exit_thread,
	[SYSCALL_CREATE_PROCESS] = syscall_create_process,

	[SYSCALL_MEMORY_ALLOCATE] = syscall_memory_allocate,

	[SYSCALL_SEND_MESSAGE] = syscall_send_message,
	[SYSCALL_RECV_MESSAGE] = syscall_recv_message,

	[SYSCALL_CHANNEL_CREATE]     = syscall_channel_create,
	[SYSCALL_CHANNEL_DESTROY]    = syscall_channel_destroy,
	[SYSCALL_CHANNEL_EVENT_RECV] = syscall_channel_event_recv,

	[SYSCALL_SIGNAL_CREATE]         = syscall_signal_create,
	[SYSCALL_SIGNAL_SEND]           = syscall_signal_send,
	[SYSCALL_SIGNAL_SEND_COALESCED] = syscall_signal_send_coalesced,
	[SYSCALL_SIGNAL_READ]           = syscall_signal_read,
	[SYSCALL_SIGNAL_WAIT]           = syscall_signal_wait,
	[SYSCALL_SIGNAL_TRY_WAIT]       = syscall_signal_try_wait,

	[SYSCALL_MODULE_RESOLVE] = syscall_module_resolve,

	[SYSCALL_CAP_CREATE]    = syscall_cap_create,
	[SYSCALL_CAP_DELEGATE]  = syscall_cap_delegate,
	[SYSCALL_CAP_DERIVE]    = syscall_cap_derive,
	[SYSCALL_CAP_CALL]      = syscall_cap_call,
	[SYSCALL_CAP_REPLY]     = syscall_cap_reply,
	[SYSCALL_CAP_REVOKE]    = syscall_cap_revoke,
	[SYSCALL_CAP_RECV]      = syscall_cap_recv,
	[SYSCALL_CAP_VALID]     = syscall_cap_valid,
	[SYSCALL_CAP_DROP]      = syscall_cap_drop,
	[SYSCALL_CAP_UNPUBLISH] = syscall_cap_unpublish,

	[SYSCALL_UPCALL_DROPPED_COUNT] = syscall_upcall_dropped_count,
};

syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNKNOWN_SYSCALL, number);
	}
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
