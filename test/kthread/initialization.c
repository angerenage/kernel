#include "test_support.h"

Test(kthread, init_reports_unsupported_context_setup) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x308000u,
		.kernel_stack_top  = 0x30c000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};

	hal_cpu_mock_set_thread_context_init_result(false);
	cr_assert_eq(thread_init_ex(&worker.thread, &params),
	             THREAD_INIT_CONTEXT_UNSUPPORTED,
	             "thread_init_ex should surface HAL bootstrap rejection");
	cr_assert(!thread_init(&worker.thread, &params), "thread_init should fail when bootstrap setup is unsupported");

	reset_test_state();
}
