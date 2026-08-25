#include <base/syscall.h>
#include <core/syscall.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>

#define SIX_ARGS uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, uintptr_t arg5

#define DEFINE_SYSCALL_STUB(function_name, marker)                                                                     \
	syscall_result_t function_name(SIX_ARGS) {                                                                         \
		(void)arg0;                                                                                                    \
		(void)arg1;                                                                                                    \
		(void)arg2;                                                                                                    \
		(void)arg3;                                                                                                    \
		(void)arg4;                                                                                                    \
		(void)arg5;                                                                                                    \
		return syscall_result_ok((uintptr_t)(marker));                                                                 \
	}

DEFINE_SYSCALL_STUB(syscall_nop, SYSCALL_NOP)
DEFINE_SYSCALL_STUB(syscall_yield, SYSCALL_YIELD)
DEFINE_SYSCALL_STUB(syscall_sleep_ms, SYSCALL_SLEEP_MS)
DEFINE_SYSCALL_STUB(syscall_tick_count, SYSCALL_TICK_COUNT)
DEFINE_SYSCALL_STUB(syscall_self, SYSCALL_SELF)
DEFINE_SYSCALL_STUB(syscall_exit_process, SYSCALL_EXIT_PROCESS)
DEFINE_SYSCALL_STUB(syscall_exit_thread, SYSCALL_EXIT_THREAD)
DEFINE_SYSCALL_STUB(syscall_create_process, SYSCALL_CREATE_PROCESS)
DEFINE_SYSCALL_STUB(syscall_memory_create, SYSCALL_MEMORY_CREATE)
DEFINE_SYSCALL_STUB(syscall_channel_create, SYSCALL_CHANNEL_CREATE)
DEFINE_SYSCALL_STUB(syscall_channel_destroy, SYSCALL_CHANNEL_DESTROY)
DEFINE_SYSCALL_STUB(syscall_channel_event_recv, SYSCALL_CHANNEL_EVENT_RECV)
DEFINE_SYSCALL_STUB(syscall_module_resolve, SYSCALL_MODULE_RESOLVE)
DEFINE_SYSCALL_STUB(syscall_signal_create, SYSCALL_SIGNAL_CREATE)
DEFINE_SYSCALL_STUB(syscall_signal_send, SYSCALL_SIGNAL_SEND)
DEFINE_SYSCALL_STUB(syscall_signal_send_coalesced, SYSCALL_SIGNAL_SEND_COALESCED)
DEFINE_SYSCALL_STUB(syscall_signal_read, SYSCALL_SIGNAL_READ)
DEFINE_SYSCALL_STUB(syscall_signal_wait, SYSCALL_SIGNAL_WAIT)
DEFINE_SYSCALL_STUB(syscall_signal_try_wait, SYSCALL_SIGNAL_TRY_WAIT)
DEFINE_SYSCALL_STUB(syscall_interrupt_attach, SYSCALL_INTERRUPT_ATTACH)
DEFINE_SYSCALL_STUB(syscall_interrupt_detach, SYSCALL_INTERRUPT_DETACH)
DEFINE_SYSCALL_STUB(syscall_cap_create, SYSCALL_CAP_CREATE)
DEFINE_SYSCALL_STUB(syscall_cap_delegate, SYSCALL_CAP_DELEGATE)
DEFINE_SYSCALL_STUB(syscall_cap_derive, SYSCALL_CAP_DERIVE)
DEFINE_SYSCALL_STUB(syscall_cap_call, SYSCALL_CAP_CALL)
DEFINE_SYSCALL_STUB(syscall_cap_reply, SYSCALL_CAP_REPLY)
DEFINE_SYSCALL_STUB(syscall_cap_revoke, SYSCALL_CAP_REVOKE)
DEFINE_SYSCALL_STUB(syscall_cap_recv, SYSCALL_CAP_RECV)
DEFINE_SYSCALL_STUB(syscall_cap_valid, SYSCALL_CAP_VALID)
DEFINE_SYSCALL_STUB(syscall_cap_drop, SYSCALL_CAP_DROP)
DEFINE_SYSCALL_STUB(syscall_cap_unpublish, SYSCALL_CAP_UNPUBLISH)
DEFINE_SYSCALL_STUB(syscall_upcall_dropped_count, SYSCALL_UPCALL_DROPPED_COUNT)

static const uintptr_t ordinary_syscalls[] = {
	SYSCALL_NOP,
	SYSCALL_YIELD,
	SYSCALL_SLEEP_MS,
	SYSCALL_TICK_COUNT,
	SYSCALL_SELF,
	SYSCALL_EXIT_PROCESS,
	SYSCALL_EXIT_THREAD,
	SYSCALL_CREATE_PROCESS,
	SYSCALL_MEMORY_CREATE,
	SYSCALL_CHANNEL_CREATE,
	SYSCALL_CHANNEL_DESTROY,
	SYSCALL_CHANNEL_EVENT_RECV,
	SYSCALL_MODULE_RESOLVE,
	SYSCALL_SIGNAL_CREATE,
	SYSCALL_SIGNAL_SEND,
	SYSCALL_SIGNAL_SEND_COALESCED,
	SYSCALL_SIGNAL_READ,
	SYSCALL_SIGNAL_WAIT,
	SYSCALL_SIGNAL_TRY_WAIT,
	SYSCALL_INTERRUPT_ATTACH,
	SYSCALL_INTERRUPT_DETACH,
	SYSCALL_CAP_CREATE,
	SYSCALL_CAP_DELEGATE,
	SYSCALL_CAP_DERIVE,
	SYSCALL_CAP_CALL,
	SYSCALL_CAP_REPLY,
	SYSCALL_CAP_REVOKE,
	SYSCALL_CAP_RECV,
	SYSCALL_CAP_VALID,
	SYSCALL_CAP_DROP,
	SYSCALL_CAP_UNPUBLISH,
	SYSCALL_UPCALL_DROPPED_COUNT,
};

_Static_assert(sizeof(ordinary_syscalls) / sizeof(ordinary_syscalls[0]) + 1u == SYSCALL_COUNT,
               "production dispatch coverage must list every ordinary syscall plus UPCALL_RETURN");

Test(syscall_dispatch, production_table_routes_every_ordinary_syscall) {
	for (size_t i = 0u; i < sizeof(ordinary_syscalls) / sizeof(ordinary_syscalls[0]); i++) {
		const uintptr_t        number = ordinary_syscalls[i];
		const syscall_result_t result = syscall_dispatch(number, 1u, 2u, 3u, 4u, 5u, 6u);

		cr_assert_eq(result.status, SYSCALL_STATUS_OK, "production table rejected syscall %zu", (size_t)number);
		cr_assert_eq(result.value, number, "production table routed syscall %zu to the wrong handler", (size_t)number);
	}
}

Test(syscall_dispatch, generic_table_leaves_upcall_return_to_frame_dispatcher) {
	const syscall_result_t result = syscall_dispatch(SYSCALL_UPCALL_RETURN, 0u, 0u, 0u, 0u, 0u, 0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_UNKNOWN_SYSCALL);
	cr_assert_eq(result.value, SYSCALL_UPCALL_RETURN);
}

Test(syscall_dispatch, rejects_numbers_outside_the_abi_table) {
	const syscall_result_t result = syscall_dispatch((uintptr_t)SYSCALL_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_UNKNOWN_SYSCALL);
	cr_assert_eq(result.value, (uintptr_t)SYSCALL_COUNT);
}
