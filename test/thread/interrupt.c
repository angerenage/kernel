#include "test_support.h"

Test(thread, interrupt_pending_flag_is_idempotent_and_clearable) {
	struct thread thread;

	irq_enable_local();
	thread_regression_init(&thread, "interrupt", 0x880000u, THREAD_PRIORITY_DEFAULT);

	cr_assert_not(thread_interrupt_pending(&thread), "fresh thread must not start interrupted");
	thread_request_interrupt(&thread);
	cr_assert(thread_interrupt_pending(&thread), "interrupt request must become visible");
	thread_request_interrupt(&thread);
	cr_assert(thread_interrupt_pending(&thread), "repeated interrupt request must remain pending");

	thread_clear_interrupt(&thread);
	cr_assert_not(thread_interrupt_pending(&thread), "clear must consume pending interrupt");
	thread_clear_interrupt(&thread);
	cr_assert_not(thread_interrupt_pending(&thread), "repeated clear must remain harmless");

	thread_regression_reset();
}

Test(thread, idle_and_terminal_threads_ignore_new_interrupt_requests) {
	struct thread idle;
	struct thread exiting;
	struct thread zombie;

	irq_enable_local();
	thread_init_idle(&idle, NULL, "idle/interrupt");
	thread_regression_init(&exiting, "exiting", 0x884000u, THREAD_PRIORITY_DEFAULT);
	thread_regression_init(&zombie, "zombie", 0x888000u, THREAD_PRIORITY_DEFAULT);
	thread_mark_exiting(&exiting, 1u);
	thread_mark_zombie(&zombie);

	thread_request_interrupt(&idle);
	thread_request_interrupt(&exiting);
	thread_request_interrupt(&zombie);

	cr_assert_not(thread_interrupt_pending(&idle), "idle thread must ignore interrupt requests");
	cr_assert_not(thread_interrupt_pending(&exiting), "EXITING thread must ignore interrupt requests");
	cr_assert_not(thread_interrupt_pending(&zombie), "ZOMBIE thread must ignore interrupt requests");

	thread_regression_reset();
}
