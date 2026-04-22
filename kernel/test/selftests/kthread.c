#include <core/cpu.h>
#include <core/kthread.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"
#include "sync_helpers.h"

#define KERNEL_SELFTEST_KTHREAD_JOIN_EXIT_CODE ((thread_exit_code_t)0x1234u)
#define KERNEL_SELFTEST_KTHREAD_CANCEL_SLEEP_MS 25u
#define KERNEL_SELFTEST_KTHREAD_TIMED_JOIN_TIMEOUT_MS 20u

struct kernel_selftest_kthread_join_state {
	struct thread* thread;
	bool           ran;
};

static void kernel_selftest_kthread_join_worker(void* arg) {
	struct kernel_selftest_kthread_join_state* state = arg;

	if (state != NULL) {
		state->thread = kthread_current();
		state->ran    = true;
	}

	kthread_exit(KERNEL_SELFTEST_KTHREAD_JOIN_EXIT_CODE);
}

static void kernel_selftest_kthread_join_waits_for_exit_and_returns_exit_code(struct kernel_selftest_context* ctx) {
	struct kthread*                           worker    = NULL;
	struct kernel_selftest_kthread_join_state state     = {0};
	thread_exit_code_t                        exit_code = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(&worker, "selftest/kthread-join", kernel_selftest_kthread_join_worker, &state),
		"failed to create join target thread",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_join(worker, &exit_code), "kthread_join failed for a live join target", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.ran, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.thread == &worker->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, exit_code == KERNEL_SELFTEST_KTHREAD_JOIN_EXIT_CODE, cleanup);
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

struct kernel_selftest_kthread_timed_join_state {
	struct kthread*    target;
	struct thread*     target_thread;
	struct thread*     joiner_thread;
	thread_exit_code_t joiner_exit_code;
	uint64_t           target_hold_ticks;
	uint64_t           target_start_tick;
	uint64_t           target_deadline_tick;
	uint64_t           joiner_start_tick;
	uint64_t           joiner_finish_tick;
	bool               target_started;
	bool               target_sleep_result;
	bool               joiner_started;
	bool               joiner_finished;
	bool               joiner_result;
};

static void kernel_selftest_kthread_timed_join_target_worker(void* arg) {
	struct kernel_selftest_kthread_timed_join_state* state = arg;

	if (state == NULL) return;

	state->target_thread        = kthread_current();
	state->target_start_tick    = sched_tick_count();
	state->target_deadline_tick = state->target_start_tick + state->target_hold_ticks;
	state->target_started       = true;
	state->target_sleep_result  = sched_sleep_until_tick(state->target_deadline_tick);
	kthread_exit(KERNEL_SELFTEST_KTHREAD_JOIN_EXIT_CODE);
}

static void kernel_selftest_kthread_timed_join_joiner_worker(void* arg) {
	struct kernel_selftest_kthread_timed_join_state* state     = arg;
	thread_exit_code_t                               exit_code = 0u;

	if (state == NULL) return;

	state->joiner_thread     = kthread_current();
	state->joiner_start_tick = sched_tick_count();
	state->joiner_started    = true;
	state->joiner_result = kthread_timed_join(state->target, KERNEL_SELFTEST_KTHREAD_TIMED_JOIN_TIMEOUT_MS, &exit_code);
	state->joiner_exit_code   = exit_code;
	state->joiner_finish_tick = sched_tick_count();
	state->joiner_finished    = true;
}

static void kernel_selftest_kthread_timed_join_times_out_without_detaching_target(struct kernel_selftest_context* ctx) {
	struct kthread*                                 target           = NULL;
	struct kthread*                                 joiner           = NULL;
	struct kernel_selftest_kthread_timed_join_state state            = {0};
	struct kernel_selftest_clock_scope              clock            = {0};
	thread_exit_code_t                              exit_code        = 0u;
	uint64_t                                        timeout_ticks    = 0u;
	uint64_t                                        timeout_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	timeout_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_KTHREAD_TIMED_JOIN_TIMEOUT_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, timeout_ticks != 0u, "timeout conversion returned zero ticks", cleanup);
	state.target_hold_ticks = timeout_ticks + KERNEL_SELFTEST_SLEEP_TICKS;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&target, "selftest/kthread-timed-target", kernel_selftest_kthread_timed_join_target_worker, &state),
		"failed to create timed join target",
		cleanup);
	state.target = target;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&joiner, "selftest/kthread-timed-joiner", kernel_selftest_kthread_timed_join_joiner_worker, &state),
		"failed to create timed joiner",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.target_started, "target thread never started", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.joiner_started, "joiner thread never started", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.target_thread == &target->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.joiner_thread == &joiner->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(target), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, target->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, target->thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, joiner->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, joiner->thread.block_reason == THREAD_BLOCK_JOIN, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&target->thread.join_wait_queue) == 1u, cleanup);

	timeout_deadline = state.joiner_start_tick + timeout_ticks;
	kernel_selftest_advance_ticks_until(timeout_deadline);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.joiner_finished, "joiner did not return from timed join", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.joiner_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.joiner_finish_tick >= timeout_deadline, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.joiner_finish_tick - state.joiner_start_tick >= timeout_ticks, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.joiner_exit_code == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&joiner->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(target), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_joinable(&target->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&target->thread.join_wait_queue) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.target_deadline_tick);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_join(target, &exit_code), "kthread_join failed for timed-join target after timeout", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, exit_code == KERNEL_SELFTEST_KTHREAD_JOIN_EXIT_CODE, cleanup);

cleanup:
	if (target != NULL && kernel_selftest_thread_is_live(target)) {
		kernel_selftest_advance_ticks_until(state.target_deadline_tick);
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (joiner != NULL && kernel_selftest_thread_is_live(joiner)) {
		(void)kthread_cancel(joiner);
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	kernel_selftest_thread_destroy(&joiner);
	kernel_selftest_thread_destroy(&target);
	kernel_selftest_clock_scope_end(&clock);
}

struct kernel_selftest_kthread_detach_state {
	uint32_t ran;
};

static void kernel_selftest_kthread_detach_worker(void* arg) {
	struct kernel_selftest_kthread_detach_state* state = arg;

	if (state != NULL) __atomic_store_n(&state->ran, 1u, __ATOMIC_RELEASE);
}

static void kernel_selftest_kthread_detach_prevents_join_but_thread_still_runs(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_kthread_detach_state state = {0};

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
	                                kthread_spawn_detached("selftest/kthread-detach",
	                                                       kernel_selftest_kthread_detach_worker,
	                                                       &state) == KTHREAD_SPAWN_OK,
	                                "failed to create detached thread",
	                                cleanup);

	for (size_t attempt = 0u; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS * cpu_count() * 1024u; attempt++) {
		if (__atomic_load_n(&state.ran, __ATOMIC_ACQUIRE) != 0u) break;
		sched_yield();
	}

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, __atomic_load_n(&state.ran, __ATOMIC_ACQUIRE) != 0u, cleanup);

cleanup:
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
}

struct kernel_selftest_kthread_cancel_state {
	uint64_t       start_tick;
	struct thread* thread;
	bool           started;
	bool           returned_from_sleep;
};

static void kernel_selftest_kthread_cancel_worker(void* arg) {
	struct kernel_selftest_kthread_cancel_state* state = arg;

	if (state == NULL) return;

	state->thread     = kthread_current();
	state->start_tick = sched_tick_count();
	state->started    = true;
	(void)kthread_sleep_ms(KERNEL_SELFTEST_KTHREAD_CANCEL_SLEEP_MS);
	state->returned_from_sleep = true;
}

static void
kernel_selftest_kthread_cancel_wakes_sleeping_thread_and_returns_cancel_code(struct kernel_selftest_context* ctx) {
	struct kthread*                             worker         = NULL;
	struct kernel_selftest_kthread_cancel_state state          = {0};
	struct kernel_selftest_clock_scope          clock          = {0};
	thread_exit_code_t                          exit_code      = 0u;
	uint64_t                                    sleep_ticks    = 0u;
	uint64_t                                    sleep_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	sleep_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_KTHREAD_CANCEL_SLEEP_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, sleep_ticks != 0u, "sleep conversion returned zero ticks", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&worker, "selftest/kthread-cancel", kernel_selftest_kthread_cancel_worker, &state),
		"failed to create cancellable thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.started, "worker thread never attempted kthread_sleep_ms", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.thread == &worker->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.returned_from_sleep, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(worker), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker->thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);

	sleep_deadline = state.start_tick + sleep_ticks;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_cancel(worker), "kthread_cancel failed for the sleeping worker", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_cancel_requested(&worker->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker->thread.state == THREAD_STATE_READY, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu_current()) >= 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_join(worker, &exit_code), "kthread_join failed for the canceled worker", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, exit_code == THREAD_EXIT_CODE_CANCELLED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.returned_from_sleep, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_tick_count() < sleep_deadline, cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(sleep_deadline);
	if (worker != NULL && kernel_selftest_thread_is_live(worker)) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL && worker != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker->thread));
	}
	kernel_selftest_thread_destroy(&worker);
	kernel_selftest_clock_scope_end(&clock);
}

static const struct kernel_selftest_case kernel_kthread_selftests[] = {
	{
     .name = "join_waits_for_exit_and_returns_exit_code",
     .run  = kernel_selftest_kthread_join_waits_for_exit_and_returns_exit_code,
	 },
	{
     .name = "timed_join_times_out_without_detaching_target",
     .run  = kernel_selftest_kthread_timed_join_times_out_without_detaching_target,
	 },
	{
     .name = "detach_prevents_join_but_thread_still_runs",
     .run  = kernel_selftest_kthread_detach_prevents_join_but_thread_still_runs,
	 },
	{
     .name = "cancel_wakes_sleeping_thread_and_returns_cancel_code",
     .run  = kernel_selftest_kthread_cancel_wakes_sleeping_thread_and_returns_cancel_code,
	 },
};

const struct kernel_selftest_suite kernel_kthread_selftest_suite = {
	.name       = "kthread",
	.cases      = kernel_kthread_selftests,
	.case_count = sizeof(kernel_kthread_selftests) / sizeof(kernel_kthread_selftests[0]),
};
