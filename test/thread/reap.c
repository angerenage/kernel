#include "test_support.h"

struct reap_probe {
	size_t         calls;
	struct thread* thread;
};

static void record_reap(struct thread* thread, void* ctx) {
	struct reap_probe* probe = ctx;

	probe->calls++;
	probe->thread = thread;
}

Test(thread, reap_callback_registration_and_notification_preserve_identity) {
	struct thread     thread;
	struct reap_probe probe = {0};

	irq_enable_local();
	thread_regression_init(&thread, "reap", 0x890000u, THREAD_PRIORITY_DEFAULT);

	thread_set_reap_callback(&thread, record_reap, &probe);
	cr_assert_eq(thread.reap_callback, record_reap, "registered callback mismatch");
	cr_assert_eq(thread.reap_context, &probe, "registered callback context mismatch");

	thread_notify_reap(&thread);
	cr_assert_eq(probe.calls, 1u, "notification must invoke callback once per call");
	cr_assert_eq(probe.thread, &thread, "callback must receive the same descriptor");

	thread_set_reap_callback(&thread, NULL, NULL);
	thread_notify_reap(&thread);
	cr_assert_eq(probe.calls, 1u, "cleared callback must not be invoked");

	thread_regression_reset();
}
