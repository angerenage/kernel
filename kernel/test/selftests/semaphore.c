#include <core/semaphore.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"
#include "sync_helpers.h"

#define KERNEL_SELFTEST_SEMAPHORE_TIMEOUT_MS 20u

static void kernel_selftest_semaphore_counts_and_overflow_behave(struct kernel_selftest_context* ctx) {
	struct semaphore semaphore;

	semaphore_init(&semaphore, 2u);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 2u, overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&semaphore) == 0u, overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_try_acquire(&semaphore), overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 1u, overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_try_acquire(&semaphore), overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 0u, overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !semaphore_try_acquire(&semaphore), overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_release(&semaphore), overflow_check);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 1u, overflow_check);

overflow_check:
	semaphore_init(&semaphore, (size_t)-1);
	KERNEL_SELFTEST_ASSERT(ctx, !semaphore_release(&semaphore));
	KERNEL_SELFTEST_ASSERT(ctx, semaphore_count(&semaphore) == (size_t)-1);
}

struct kernel_selftest_semaphore_wait_state {
	struct semaphore semaphore;
	struct thread*   waiter_thread;
	bool             waiter_started;
	bool             waiter_acquired;
};

static void kernel_selftest_semaphore_waiter_worker(void* arg) {
	struct kernel_selftest_semaphore_wait_state* state = arg;

	if (state == NULL) return;

	state->waiter_thread  = kthread_current();
	state->waiter_started = true;
	semaphore_acquire(&state->semaphore);
	state->waiter_acquired = true;
}

static void kernel_selftest_semaphore_release_wakes_blocked_waiter(struct kernel_selftest_context* ctx) {
	struct kthread*                             waiter = NULL;
	struct kernel_selftest_semaphore_wait_state state  = {0};

	semaphore_init(&state.semaphore, 0u);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&waiter, "selftest/semaphore-waiter", kernel_selftest_semaphore_waiter_worker, &state),
		"failed to create semaphore waiter thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, state.waiter_started, "waiter thread did not attempt semaphore_acquire", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiter), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_thread == &waiter->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.block_reason == THREAD_BLOCK_SEMAPHORE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&state.semaphore) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&state.semaphore) == 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, semaphore_release(&state.semaphore), "semaphore_release failed to publish a permit", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&state.semaphore) == 1u, cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&state.semaphore) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&state.semaphore) == 0u, cleanup);

cleanup:
	if (kernel_selftest_thread_is_live(waiter) && semaphore_waiter_count(&state.semaphore) != 0u) {
		(void)semaphore_release(&state.semaphore);
	}
	if (kernel_selftest_thread_is_live(waiter)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && waiter != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter->thread));
	}
	kernel_selftest_thread_destroy(&waiter);
}

struct kernel_selftest_semaphore_timed_state {
	struct semaphore semaphore;
	struct thread*   waiter_thread;
	uint64_t         waiter_start_tick;
	uint64_t         waiter_finish_tick;
	bool             waiter_started;
	bool             waiter_result;
	bool             waiter_finished;
};

static void kernel_selftest_semaphore_timed_waiter_worker(void* arg) {
	struct kernel_selftest_semaphore_timed_state* state = arg;

	if (state == NULL) return;

	state->waiter_thread      = kthread_current();
	state->waiter_started     = true;
	state->waiter_start_tick  = sched_tick_count();
	state->waiter_result      = semaphore_timed_acquire(&state->semaphore, KERNEL_SELFTEST_SEMAPHORE_TIMEOUT_MS);
	state->waiter_finish_tick = sched_tick_count();
	state->waiter_finished    = true;
}

static void kernel_selftest_semaphore_timed_acquire_times_out_without_permit(struct kernel_selftest_context* ctx) {
	struct kthread*                              waiter           = NULL;
	struct kernel_selftest_semaphore_timed_state state            = {0};
	struct kernel_selftest_clock_scope           clock            = {0};
	uint64_t                                     timeout_ticks    = 0u;
	uint64_t                                     timeout_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	timeout_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_SEMAPHORE_TIMEOUT_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, timeout_ticks != 0u, "timeout conversion returned zero ticks", cleanup);

	semaphore_init(&state.semaphore, 0u);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&waiter, "selftest/semaphore-timeout", kernel_selftest_semaphore_timed_waiter_worker, &state),
		"failed to create timed semaphore waiter thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, state.waiter_started, "waiter thread did not attempt semaphore_timed_acquire", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiter), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_thread == &waiter->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.block_reason == THREAD_BLOCK_SEMAPHORE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&state.semaphore) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&state.semaphore) == 0u, cleanup);

	timeout_deadline = state.waiter_start_tick + timeout_ticks;
	kernel_selftest_advance_ticks_until(timeout_deadline);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick >= timeout_deadline, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick - state.waiter_start_tick >= timeout_ticks, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&state.semaphore) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&state.semaphore) == 0u, cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(timeout_deadline);
	if (kernel_selftest_thread_is_live(waiter)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && waiter != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter->thread));
	}
	kernel_selftest_thread_destroy(&waiter);
	kernel_selftest_clock_scope_end(&clock);
}

static void kernel_selftest_semaphore_timed_acquire_zero_timeout_is_non_blocking(struct kernel_selftest_context* ctx) {
	struct semaphore semaphore;

	semaphore_init(&semaphore, 1u);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_timed_acquire(&semaphore, 0u), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !semaphore_timed_acquire(&semaphore, 0u), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_count(&semaphore) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, semaphore_waiter_count(&semaphore) == 0u, cleanup);

cleanup:
	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, semaphore_count(&semaphore) == 0u);
		KERNEL_SELFTEST_ASSERT(ctx, semaphore_waiter_count(&semaphore) == 0u);
	}
}

static const struct kernel_selftest_case kernel_semaphore_selftests[] = {
	{
     .name = "counts_and_overflow_behave",
     .run  = kernel_selftest_semaphore_counts_and_overflow_behave,
	 },
	{
     .name = "release_wakes_blocked_waiter",
     .run  = kernel_selftest_semaphore_release_wakes_blocked_waiter,
	 },
	{
     .name = "timed_acquire_times_out_without_permit",
     .run  = kernel_selftest_semaphore_timed_acquire_times_out_without_permit,
	 },
	{
     .name = "timed_acquire_zero_timeout_is_non_blocking",
     .run  = kernel_selftest_semaphore_timed_acquire_zero_timeout_is_non_blocking,
	 },
};

const struct kernel_selftest_suite kernel_semaphore_selftest_suite = {
	.name       = "semaphore",
	.cases      = kernel_semaphore_selftests,
	.case_count = sizeof(kernel_semaphore_selftests) / sizeof(kernel_semaphore_selftests[0]),
};
