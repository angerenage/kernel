#include "test_support.h"

Test(syscall, exit_thread_requires_current_userspace_thread) {
	syscall_result_t result;

	syscall_test_init_process_environment();
	result = syscall_dispatch(SYSCALL_EXIT_THREAD, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	syscall_test_reset_state();
}

Test(syscall, yield_dispatches_next_runnable_thread) {
	const struct thread_create_params first_params = {
		.name              = "syscall-yield-first",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "syscall-yield-second",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	syscall_test_init_scheduler();
	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &first, "yield should dispatch first runnable thread");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &second, "yield should rotate to the next runnable thread");
	cr_assert(thread_is_queued(&first), "previous thread should be queued after yielding");

	syscall_test_reset_state();
}
