#include "test_support.h"

Test(syscall, nop_returns_ok) {
	syscall_result_t result = syscall_dispatch(SYSCALL_NOP, 1u, 2u, 3u, 4u, 5u, 6u);

	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert(syscall_status_is_success(result.status), "nop should return success");
}

Test(syscall, invalid_number_returns_unknown_syscall) {
	syscall_result_t result = syscall_dispatch(UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u, 0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_UNKNOWN_SYSCALL);
	cr_assert_eq(result.value, UINTPTR_MAX);
	cr_assert(syscall_status_is_caller_error(result.status), "unknown syscall should be a caller error");
}
