#include <base/time.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <hal/clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
	SYSCALL_COUNT   = 3u,
	SYSCALL_INVALID = UINTPTR_MAX,
};

static uintptr_t syscall_nop(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
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

	return 0u;
}

static uintptr_t syscall_yield(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                               uintptr_t arg5) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	sched_yield();
	return 0u;
}

static uintptr_t syscall_sleep_ms(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t arg5) {
	uint64_t deadline_tick;

	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	if (arg0 == 0u) {
		sched_yield();
		return 0u;
	}

	if (!time_tick_deadline_from_ms(sched_tick_count(), (uint64_t)arg0, hal_clock_frequency(), &deadline_tick)) {
		return SYSCALL_INVALID;
	}
	if (!sched_sleep_until_tick(deadline_tick)) return SYSCALL_INVALID;
	return 0u;
}

static syscall_fn_t syscall_table[SYSCALL_COUNT] = {
	[SYSCALL_NOP]      = syscall_nop,
	[SYSCALL_YIELD]    = syscall_yield,
	[SYSCALL_SLEEP_MS] = syscall_sleep_ms,
};

uintptr_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                           uintptr_t arg4, uintptr_t arg5) {
	if (number >= SYSCALL_COUNT || syscall_table[number] == NULL) return SYSCALL_INVALID;
	return syscall_table[number](arg0, arg1, arg2, arg3, arg4, arg5);
}
