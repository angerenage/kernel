#include <stddef.h>
#include <stdint.h>
#include <system/time.h>

#include "syscall.h"

bool sched_yield(void) {
	syscall_result_t result = syscall(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool sleep_ms(size_t milliseconds) {
	syscall_result_t result = syscall(SYSCALL_SLEEP_MS, (uintptr_t)milliseconds, 0u, 0u, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool tick_count(uint64_t* out_ticks) {
	syscall_result_t result = syscall(SYSCALL_TICK_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);

	if (out_ticks == NULL) return false;
	if (result.status != SYSCALL_STATUS_OK) return false;
	*out_ticks = (uint64_t)result.value;
	return true;
}
