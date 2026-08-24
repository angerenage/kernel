#include <base/syscall.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/upcall.h>

#include "syscall.h"

static __attribute__((noreturn))
void upcall_hcf(void) {
	printf("userspace: upcall return syscall unexpectedly returned; halting intentionally\n");
	for (;;) {
	}
}

syscall_status_t upcall_dropped_count(uint64_t* out_count) {
	syscall_result_t result;

	if (out_count == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_count);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = syscall(SYSCALL_UPCALL_DROPPED_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_UPCALL_DROPPED_COUNT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	*out_count = (uint64_t)result.value;
	return SYSCALL_STATUS_OK;
}

__attribute__((noreturn))
void upcall_return(void) {
	(void)syscall(SYSCALL_UPCALL_RETURN, 0u, 0u, 0u, 0u, 0u, 0u);
	upcall_hcf();
}
