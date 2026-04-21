#include <core/cpu.h>
#include <core/kthread.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"
#include "sync_helpers.h"

struct kernel_selftest_thread_bootstrap_state {
	struct cpu*    cpu;
	struct thread* thread;
	uintptr_t      arg_value;
	bool           ran;
};

static const char* kernel_selftest_thread_bootstrap_create_failure_message(enum thread_init_result result) {
	switch (result) {
	case THREAD_INIT_INVALID_STACK:
		return "stack allocation produced invalid thread bounds";
	case THREAD_INIT_CONTEXT_UNSUPPORTED:
		return "hal_cpu_thread_context_init rejected bootstrap setup";
	case THREAD_INIT_INVALID_ARGUMENTS:
		return "thread bootstrap worker parameters were rejected";
	case THREAD_INIT_OK:
	default:
		return "thread bootstrap worker creation failed";
	}
}

static void kernel_selftest_thread_bootstrap_worker(void* arg) {
	struct kernel_selftest_thread_bootstrap_state* state = arg;

	if (state == NULL) return;

	state->cpu       = cpu_current();
	state->thread    = kthread_current();
	state->arg_value = (uintptr_t)arg;
	state->ran       = true;
}

static void kernel_selftest_thread_bootstrap_create_start_join_worker(struct kernel_selftest_context* ctx) {
	struct vmm_alloc_params stack_params = {
		.page_count  = KERNEL_SELFTEST_THREAD_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct thread_create_params           params;
	struct kernel_selftest_managed_thread worker = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_thread_bootstrap_state state      = {0};
	struct cpu*                                   cpu        = cpu_current();
	void*                                         stack_base = NULL;
	thread_exit_code_t                            exit_code  = THREAD_EXIT_CODE_CANCELLED;
	enum thread_init_result                       create_result;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.stack_id == VMM_ID_INVALID, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                vmm_alloc(&stack_params, &worker.stack_id, &stack_base),
	                                "failed to allocate thread bootstrap stack",
	                                cleanup);
	params = (struct thread_create_params){
		.name              = "selftest/thread-bootstrap",
		.entry             = kernel_selftest_thread_bootstrap_worker,
		.arg               = &state,
		.kernel_stack_base = (uintptr_t)stack_base,
		.kernel_stack_top  = (uintptr_t)stack_base + KERNEL_SELFTEST_THREAD_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE,
		.preferred_cpu     = cpu,
		.detached          = false,
	};

	create_result = kthread_create_ex(&worker.thread, &params);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                create_result == THREAD_INIT_OK,
	                                kernel_selftest_thread_bootstrap_create_failure_message(create_result),
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.state == THREAD_STATE_NEW, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.preferred_cpu == cpu, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&worker.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&worker.thread), "failed to start thread bootstrap worker", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_join(&worker.thread, &exit_code), "failed to join thread bootstrap worker", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.ran, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.cpu == cpu, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.thread == &worker.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.arg_value == (uintptr_t)&state, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, exit_code == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&worker.thread.join_wait_queue) == 0u, cleanup);

cleanup:
	if (!thread_is_terminated(&worker.thread)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && worker.stack_id != VMM_ID_INVALID) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker.thread));
	}
	kernel_selftest_thread_destroy(&worker);
}

static const struct kernel_selftest_case kernel_thread_bootstrap_selftests[] = {
	{
     .name = "create_start_join_worker",
     .run  = kernel_selftest_thread_bootstrap_create_start_join_worker,
	 },
};

const struct kernel_selftest_suite kernel_thread_bootstrap_selftest_suite = {
	.name       = "thread_bootstrap",
	.cases      = kernel_thread_bootstrap_selftests,
	.case_count = sizeof(kernel_thread_bootstrap_selftests) / sizeof(kernel_thread_bootstrap_selftests[0]),
};
