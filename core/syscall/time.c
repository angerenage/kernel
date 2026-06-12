#include <base/time.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <hal/clock.h>
#include <stdint.h>

syscall_result_t syscall_yield(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
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

syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
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

syscall_result_t syscall_tick_count(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	return syscall_result_ok((uintptr_t)sched_tick_count());
}
