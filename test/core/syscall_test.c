#include <core/syscall.h>
#include <criterion/criterion.h>
#include <stdint.h>

Test(syscall, nop_returns_zero) {
	cr_assert_eq(syscall_dispatch(SYSCALL_NOP, 1u, 2u, 3u, 4u, 5u, 6u), 0u);
}

Test(syscall, invalid_number_returns_invalid_word) {
	cr_assert_eq(syscall_dispatch(UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u, 0u), UINTPTR_MAX);
}
