#include "test_support.h"

Test(thread, cancellation_flags_are_idempotent_and_terminal_threads_reject_requests) {
	struct thread thread;

	irq_enable_local();
	thread_regression_init(&thread, "cancel", 0x870000u, THREAD_PRIORITY_DEFAULT);

	thread_set_cancel_enabled(&thread, false);
	cr_assert_not(thread_cancel_enabled(&thread), "cancellation must be disabled");
	cr_assert(thread_request_cancel(&thread), "first cancellation request must succeed");
	cr_assert(thread_request_cancel(&thread), "repeated cancellation request must remain harmless");
	cr_assert(thread_cancel_requested(&thread), "pending cancellation flag must remain set");
	cr_assert_not(thread_should_cancel(&thread), "disabled cancellation must remain masked");

	thread_set_cancel_enabled(&thread, true);
	cr_assert(thread_cancel_enabled(&thread), "cancellation must be re-enabled");
	cr_assert(thread_should_cancel(&thread), "pending request must become actionable again");

	thread_mark_exiting(&thread, THREAD_EXIT_CODE_CANCELLED);
	cr_assert_not(thread_request_cancel(&thread), "EXITING thread must reject new cancellation");
	thread_mark_zombie(&thread);
	cr_assert_not(thread_request_cancel(&thread), "ZOMBIE thread must reject new cancellation");

	thread_regression_reset();
}

Test(thread, idle_thread_never_becomes_cancellable) {
	struct thread idle;

	irq_enable_local();
	thread_init_idle(&idle, NULL, "idle/cancel");

	thread_set_cancel_enabled(&idle, false);
	cr_assert_not(thread_request_cancel(&idle), "idle thread must reject cancellation request");
	cr_assert_not(thread_cancel_requested(&idle), "idle thread must not gain pending cancellation");
	cr_assert_not(thread_should_cancel(&idle), "idle thread must never become cancellable");

	thread_set_cancel_enabled(&idle, true);
	cr_assert_not(thread_should_cancel(&idle), "unmasking cancellation must not affect idle thread");

	thread_regression_reset();
}
