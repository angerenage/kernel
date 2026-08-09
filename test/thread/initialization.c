#include "test_support.h"

Test(thread, init_populates_extended_descriptor_fields) {
	int                               arg    = 42;
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = thread_test_entry,
		.arg               = &arg,
		.kernel_stack_base = 0x400000u,
		.kernel_stack_top  = 0x404000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread thread;

	cr_assert(thread_init(&thread, &params), "thread_init rejected valid params");
	cr_assert_eq(thread_init_ex(&thread, &params), THREAD_INIT_OK, "thread_init_ex rejected valid params");
	cr_assert_str_eq(thread.name, "worker", "thread name mismatch");
	cr_assert_eq(thread.state, THREAD_STATE_NEW, "new thread should start in NEW state");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "new thread should not start blocked");
	cr_assert_eq(thread.kernel_stack_base, params.kernel_stack_base, "stack base mismatch");
	cr_assert_eq(thread.kernel_stack_top, params.kernel_stack_top, "stack top mismatch");
	cr_assert_neq(thread.context.instruction_pointer, 0u, "initial instruction pointer should be populated");
	cr_assert(thread.context.stack_pointer <= params.kernel_stack_top, "initial stack pointer should stay in range");
	cr_assert(thread.context.stack_pointer > params.kernel_stack_base, "initial stack pointer should stay in range");
	cr_assert_eq(thread.entry, thread_test_entry, "entry pointer mismatch");
	cr_assert_eq(thread.arg, &arg, "thread arg mismatch");
	cr_assert_eq(
		thread.timeslice_ticks, THREAD_DEFAULT_TIMESLICE_TICKS, "regular threads should inherit the default timeslice");
	cr_assert_eq(thread.timeslice_remaining,
	             THREAD_DEFAULT_TIMESLICE_TICKS,
	             "regular threads should start with a full timeslice budget");
	cr_assert_eq(thread.base_priority, THREAD_PRIORITY_DEFAULT, "regular threads should inherit the default priority");
	cr_assert_eq(thread.effective_priority,
	             THREAD_PRIORITY_DEFAULT,
	             "regular threads should start with base priority as effective priority");
	cr_assert(thread_is_joinable(&thread), "fresh non-detached thread should be joinable");
	cr_assert(!thread_is_terminated(&thread), "fresh thread should not be terminated");
	cr_assert_eq(thread_wait_queue_depth(&thread.join_wait_queue), 0u, "join wait queue should start empty");
	cr_assert_eq(thread_wait_queue_depth(&thread.park_wait_queue), 0u, "park wait queue should start empty");
	cr_assert_eq(thread.flags & THREAD_FLAG_PARK_PERMIT, 0u, "fresh thread should not start with a park permit");
}

Test(thread, init_rejects_invalid_regular_thread_params) {
	struct thread thread;

	cr_assert_eq(
		thread_init_ex(NULL, NULL), THREAD_INIT_INVALID_ARGUMENTS, "NULL thread and params should be rejected");
	cr_assert_eq(thread_init_ex(&thread, NULL), THREAD_INIT_INVALID_ARGUMENTS, "NULL params should be rejected");
	cr_assert_eq(thread_init_ex(&thread,
	                            &(const struct thread_create_params){
									.name              = "bad",
									.entry             = NULL,
									.arg               = NULL,
									.kernel_stack_base = 0x500000u,
									.kernel_stack_top  = 0x504000u,
									.preferred_cpu     = NULL,
									.detached          = false,
								}),
	             THREAD_INIT_INVALID_ARGUMENTS,
	             "NULL entry should be rejected");
	cr_assert_eq(thread_init_ex(&thread,
	                            &(const struct thread_create_params){
									.name              = "bad",
									.entry             = thread_test_entry,
									.arg               = NULL,
									.kernel_stack_base = 0x504000u,
									.kernel_stack_top  = 0x504000u,
									.preferred_cpu     = NULL,
									.detached          = false,
								}),
	             THREAD_INIT_INVALID_STACK,
	             "empty stack range should be rejected");
	cr_assert(!thread_init(&thread,
	                       &(const struct thread_create_params){
							   .name              = "bad",
							   .entry             = thread_test_entry,
							   .arg               = NULL,
							   .kernel_stack_base = 0x504000u,
							   .kernel_stack_top  = 0x504000u,
							   .preferred_cpu     = NULL,
							   .detached          = false,
						   }),
	          "thread_init should reject an empty stack range");
}

Test(thread, init_reports_unsupported_context_setup) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x520000u,
		.kernel_stack_top  = 0x524000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread thread = {0};

	hal_cpu_mock_set_thread_context_init_result(false);
	cr_assert_eq(thread_init_ex(&thread, &params),
	             THREAD_INIT_CONTEXT_UNSUPPORTED,
	             "thread_init_ex should surface HAL bootstrap rejection");
	cr_assert(!thread_init(&thread, &params), "thread_init should fail when the HAL rejects bootstrap setup");
	reset_test_state();
}

Test(thread, priority_inputs_are_clamped_at_descriptor_creation) {
	struct thread too_low;
	struct thread too_high;

	irq_enable_local();
	thread_regression_init(&too_low, "too_low", 0x840000u, THREAD_PRIORITY_MIN - 100);
	thread_regression_init(&too_high, "too_high", 0x844000u, THREAD_PRIORITY_MAX + 100);

	cr_assert_eq(too_low.base_priority, THREAD_PRIORITY_MIN, "base priority must clamp to minimum");
	cr_assert_eq(too_low.effective_priority, THREAD_PRIORITY_MIN, "effective priority must clamp to minimum");
	cr_assert_eq(too_high.base_priority, THREAD_PRIORITY_MAX, "base priority must clamp to maximum");
	cr_assert_eq(too_high.effective_priority, THREAD_PRIORITY_MAX, "effective priority must clamp to maximum");
	cr_assert_eq(thread_priority_clamp(THREAD_PRIORITY_MIN - 1), THREAD_PRIORITY_MIN, "minimum clamp mismatch");
	cr_assert_eq(thread_priority_clamp(THREAD_PRIORITY_MAX + 1), THREAD_PRIORITY_MAX, "maximum clamp mismatch");
	cr_assert_eq(thread_priority_clamp(THREAD_PRIORITY_DEFAULT),
	             THREAD_PRIORITY_DEFAULT,
	             "in-range priority must remain unchanged");

	thread_regression_reset();
}
