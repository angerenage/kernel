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

static const char* kernel_selftest_thread_bootstrap_spawn_failure_message(enum kthread_spawn_result result) {
	switch (result) {
	case KTHREAD_SPAWN_CONTEXT_UNSUPPORTED:
		return "hal_cpu_thread_context_init rejected bootstrap setup";
	case KTHREAD_SPAWN_INVALID_ARGUMENTS:
		return "thread bootstrap worker parameters were rejected";
	case KTHREAD_SPAWN_NO_MEMORY:
		return "thread bootstrap worker allocation failed";
	case KTHREAD_SPAWN_STACK_ALLOC_FAILED:
		return "thread bootstrap stack allocation failed";
	case KTHREAD_SPAWN_START_FAILED:
		return "thread bootstrap worker could not be made runnable";
	case KTHREAD_SPAWN_REAPER_UNAVAILABLE:
		return "thread bootstrap reaper unavailable";
	case KTHREAD_SPAWN_OK:
	default:
		return "thread bootstrap worker spawn failed";
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
	struct kthread*                               worker    = NULL;
	struct kernel_selftest_thread_bootstrap_state state     = {0};
	struct cpu*                                   cpu       = cpu_current();
	thread_exit_code_t                            exit_code = THREAD_EXIT_CODE_CANCELLED;
	enum kthread_spawn_result                     spawn_result;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);

	spawn_result = kthread_spawn_on_cpu(
		&worker, "selftest/thread-bootstrap", kernel_selftest_thread_bootstrap_worker, &state, cpu);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                spawn_result == KTHREAD_SPAWN_OK,
	                                kernel_selftest_thread_bootstrap_spawn_failure_message(spawn_result),
	                                cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker->thread.preferred_cpu == cpu, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(worker), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_join(worker, &exit_code), "failed to join thread bootstrap worker", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.ran, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.cpu == cpu, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.thread == &worker->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.arg_value == (uintptr_t)&state, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, exit_code == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&worker->thread.join_wait_queue) == 0u, cleanup);

cleanup:
	if (worker != NULL && kernel_selftest_thread_is_live(worker)) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL && worker != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker->thread));
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
