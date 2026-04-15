#include <core/cpu.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

static void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_local_bind(NULL);
}

static void thread_test_entry(void* arg) {
	(void)arg;
}

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
	cr_assert(thread_is_joinable(&thread), "fresh non-detached thread should be joinable");
	cr_assert(!thread_is_terminated(&thread), "fresh thread should not be terminated");
	cr_assert_eq(thread_wait_queue_depth(&thread.join_wait_queue), 0u, "join wait queue should start empty");
}

Test(thread, init_rejects_invalid_regular_thread_params) {
	struct thread thread;

	cr_assert(!thread_init(NULL, NULL), "NULL thread and params should be rejected");
	cr_assert(!thread_init(&thread, NULL), "NULL params should be rejected");
	cr_assert(!thread_init(&thread,
	                       &(const struct thread_create_params){
							   .name              = "bad",
							   .entry             = NULL,
							   .arg               = NULL,
							   .kernel_stack_base = 0x500000u,
							   .kernel_stack_top  = 0x504000u,
							   .preferred_cpu     = NULL,
							   .detached          = false,
						   }),
	          "NULL entry should be rejected");
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
	          "empty stack range should be rejected");
}

Test(thread, lifecycle_helpers_update_state_flags_and_links) {
	const struct thread_create_params params = {
		.name              = "lifecycle",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x600000u,
		.kernel_stack_top  = 0x604000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread thread;

	init_bound_bootstrap_cpu();
	cr_assert(thread_init(&thread, &params), "thread_init failed");

	thread.flags |= THREAD_FLAG_QUEUED;
	thread.run_queue_next  = &thread;
	thread.wait_queue_next = &thread;
	thread_mark_ready(&thread, cpu_current());
	cr_assert_eq(thread.state, THREAD_STATE_READY, "thread_mark_ready should move thread to READY");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_ready should clear block reason");
	cr_assert_eq(thread.cpu, cpu_current(), "thread_mark_ready should bind target cpu");
	cr_assert(!thread_is_queued(&thread), "thread_mark_ready should clear queued flag");
	cr_assert_null(thread.run_queue_next, "thread_mark_ready should clear run queue linkage");
	cr_assert_null(thread.wait_queue_next, "thread_mark_ready should clear wait queue linkage");

	thread.flags |= THREAD_FLAG_QUEUED;
	thread.run_queue_next  = &thread;
	thread.wait_queue_next = &thread;
	thread_mark_blocked(&thread, THREAD_BLOCK_JOIN);
	cr_assert_eq(thread.state, THREAD_STATE_BLOCKED, "thread_mark_blocked should move thread to BLOCKED");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_JOIN, "thread_mark_blocked should record block reason");
	cr_assert(!thread_is_queued(&thread), "thread_mark_blocked should clear queued flag");
	cr_assert_null(thread.run_queue_next, "thread_mark_blocked should clear run queue linkage");
	cr_assert_null(thread.wait_queue_next, "thread_mark_blocked should clear wait queue linkage");

	thread_mark_running(&thread, cpu_current());
	cr_assert_eq(thread.state, THREAD_STATE_RUNNING, "thread_mark_running should move thread to RUNNING");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_running should clear block reason");

	cr_assert(thread_request_cancel(&thread), "thread_request_cancel should succeed on live thread");
	cr_assert(thread_cancel_requested(&thread), "cancel request flag should be visible");
	thread_set_cancel_enabled(&thread, false);
	cr_assert((thread.flags & THREAD_FLAG_CANCEL_DISABLED) != 0u, "cancel disable flag should be set");
	thread_set_cancel_enabled(&thread, true);
	cr_assert((thread.flags & THREAD_FLAG_CANCEL_DISABLED) == 0u, "cancel disable flag should clear");

	thread_mark_exiting(&thread, 99u);
	cr_assert_eq(thread.state, THREAD_STATE_EXITING, "thread_mark_exiting should move thread to EXITING");
	cr_assert_eq(thread.exit_code, 99u, "thread_mark_exiting should publish exit code");
	cr_assert(thread_is_terminated(&thread), "EXITING thread should count as terminated");
	cr_assert(!thread_request_cancel(&thread), "terminated thread should reject new cancel requests");

	thread_mark_zombie(&thread);
	cr_assert_eq(thread.state, THREAD_STATE_ZOMBIE, "thread_mark_zombie should move thread to ZOMBIE");
	cr_assert_eq(thread.block_reason, THREAD_BLOCK_NONE, "thread_mark_zombie should clear block reason");

	reset_test_state();
}

Test(thread, detach_and_idle_helpers_follow_joinability_rules) {
	const struct thread_create_params detached_params = {
		.name              = "detached",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x700000u,
		.kernel_stack_top  = 0x704000u,
		.preferred_cpu     = NULL,
		.detached          = true,
	};
	const struct thread_create_params joinable_params = {
		.name              = "joinable",
		.entry             = thread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x710000u,
		.kernel_stack_top  = 0x714000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread detached_thread;
	struct thread joinable_thread;
	struct thread idle_thread;

	init_bound_bootstrap_cpu();

	cr_assert(thread_init(&detached_thread, &detached_params), "detached thread_init failed");
	cr_assert(!thread_is_joinable(&detached_thread), "thread created detached should not be joinable");

	cr_assert(thread_init(&joinable_thread, &joinable_params), "joinable thread_init failed");
	cr_assert(thread_is_joinable(&joinable_thread), "fresh thread should be joinable");
	cr_assert(thread_detach(&joinable_thread), "thread_detach should succeed once");
	cr_assert(!thread_is_joinable(&joinable_thread), "detached thread should stop being joinable");
	cr_assert(!thread_detach(&joinable_thread), "thread_detach should fail once already detached");

	thread_init_idle(&idle_thread, cpu_current(), "idle/test");
	cr_assert(thread_is_idle(&idle_thread), "idle thread should carry idle flag");
	cr_assert(!thread_is_joinable(&idle_thread), "idle thread must never be joinable");
	cr_assert(!thread_request_cancel(&idle_thread), "idle thread must reject cancellation");
	cr_assert(!thread_detach(&idle_thread), "idle thread must reject detach");
	cr_assert_eq(idle_thread.timeslice_ticks, 0u, "idle threads should not consume scheduler timeslices");
	cr_assert_eq(idle_thread.timeslice_remaining, 0u, "idle threads should not carry a timeslice budget");
	cr_assert_eq(
		thread_wait_queue_depth(&idle_thread.join_wait_queue), 0u, "idle thread wait queue should start empty");

	reset_test_state();
}
