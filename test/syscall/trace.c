#include <base/trace.h>
#include <core/syscall.h>
#include <criterion/criterion.h>
#include <stdint.h>

Test(syscall, trace_register_validates_arguments_when_tracing_is_disabled) {
	const char       name[] = "test";
	syscall_result_t result;

	result = syscall_dispatch(SYSCALL_TRACE_REGISTER, 0u, sizeof(name) - 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_TRACE_REGISTER, (uintptr_t)name, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_TRACE_REGISTER, (uintptr_t)name, sizeof(name) - 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
}

Test(syscall, trace_event_validates_type_and_label) {
	syscall_result_t result;

	result = syscall_dispatch(SYSCALL_TRACE_EVENT, TRACE_EVENT_END + 1u, 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_TRACE_EVENT, TRACE_EVENT_INSTANT, TRACE_LABEL_ID_INVALID, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_TRACE_EVENT, TRACE_EVENT_INSTANT, 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
}
