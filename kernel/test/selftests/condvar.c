#include <core/condvar.h>
#include <core/mutex.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"
#include "sync_helpers.h"

#define KERNEL_SELFTEST_CONDVAR_TIMEOUT_MS 20u

static void kernel_selftest_condvar_empty_queue_signal_and_broadcast_are_noops(struct kernel_selftest_context* ctx) {
	struct condvar condvar;

	condvar_init(&condvar);
	KERNEL_SELFTEST_ASSERT(ctx, thread_wait_queue_depth(&condvar.waiters) == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, !condvar_signal(&condvar));
	KERNEL_SELFTEST_ASSERT(ctx, condvar_broadcast(&condvar) == 0u);
}

struct kernel_selftest_condvar_signal_state {
	struct condvar condvar;
	struct mutex   mutex;
	struct thread* waiter_thread;
	bool           waiter_started;
	bool           waiter_owned_mutex_before_wait;
	bool           waiter_resumed;
	bool           waiter_owned_mutex_after_wait;
	bool           waiter_unlocked;
	bool           signaler_locked_mutex;
	bool           signaler_signaled;
};

static void kernel_selftest_condvar_waiter_worker(void* arg) {
	struct kernel_selftest_condvar_signal_state* state = arg;

	if (state == NULL) return;

	mutex_lock(&state->mutex);
	state->waiter_thread                  = kthread_current();
	state->waiter_started                 = true;
	state->waiter_owned_mutex_before_wait = mutex_is_owned_by_current(&state->mutex);
	condvar_wait(&state->condvar, &state->mutex);
	state->waiter_resumed                = true;
	state->waiter_owned_mutex_after_wait = mutex_is_owned_by_current(&state->mutex);
	state->waiter_unlocked               = mutex_unlock(&state->mutex);
}

static void
kernel_selftest_condvar_wait_releases_mutex_and_reacquires_after_signal(struct kernel_selftest_context* ctx) {
	struct kthread*                             waiter = NULL;
	struct kernel_selftest_condvar_signal_state state  = {0};

	condvar_init(&state.condvar);
	mutex_init(&state.mutex);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&waiter, "selftest/condvar-waiter", kernel_selftest_condvar_waiter_worker, &state),
		"failed to create condvar waiter thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.waiter_started, "waiter thread did not enter condvar_wait", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex_before_wait, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_resumed, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiter), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_thread == &waiter->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.block_reason == THREAD_BLOCK_WAIT_QUEUE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, mutex_try_lock(&state.mutex), "condvar_wait did not release the mutex", cleanup);

	state.signaler_locked_mutex = true;
	state.signaler_signaled     = condvar_signal(&state.condvar);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.signaler_signaled, "condvar_signal failed to wake the waiter", unlock);

unlock:
	if (state.signaler_locked_mutex) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_unlock(&state.mutex), cleanup);
		state.signaler_locked_mutex = false;
	}

	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_resumed, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex_after_wait, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);

cleanup:
	if (state.signaler_locked_mutex) (void)mutex_unlock(&state.mutex);
	if (kernel_selftest_thread_is_live(waiter) && mutex_try_lock(&state.mutex)) {
		(void)condvar_signal(&state.condvar);
		(void)mutex_unlock(&state.mutex);
	}
	if (kernel_selftest_thread_is_live(waiter)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && waiter != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter->thread));
	}
	kernel_selftest_thread_destroy(&waiter);
}

struct kernel_selftest_condvar_timed_state {
	struct condvar condvar;
	struct mutex   mutex;
	struct thread* waiter_thread;
	uint64_t       waiter_start_tick;
	uint64_t       waiter_finish_tick;
	bool           waiter_started;
	bool           waiter_result;
	bool           waiter_finished;
	bool           waiter_owned_mutex_after_wait;
	bool           waiter_unlocked;
};

static void kernel_selftest_condvar_timed_waiter_worker(void* arg) {
	struct kernel_selftest_condvar_timed_state* state = arg;

	if (state == NULL) return;

	mutex_lock(&state->mutex);
	state->waiter_thread      = kthread_current();
	state->waiter_started     = true;
	state->waiter_start_tick  = sched_tick_count();
	state->waiter_result      = condvar_timed_wait(&state->condvar, &state->mutex, KERNEL_SELFTEST_CONDVAR_TIMEOUT_MS);
	state->waiter_finish_tick = sched_tick_count();
	state->waiter_finished    = true;
	state->waiter_owned_mutex_after_wait = mutex_is_owned_by_current(&state->mutex);
	state->waiter_unlocked               = mutex_unlock(&state->mutex);
}

static void kernel_selftest_condvar_timed_wait_times_out_and_reacquires_mutex(struct kernel_selftest_context* ctx) {
	struct kthread*                            waiter           = NULL;
	struct kernel_selftest_condvar_timed_state state            = {0};
	struct kernel_selftest_clock_scope         clock            = {0};
	uint64_t                                   timeout_ticks    = 0u;
	uint64_t                                   timeout_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	timeout_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_CONDVAR_TIMEOUT_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, timeout_ticks != 0u, "timeout conversion returned zero ticks", cleanup);

	condvar_init(&state.condvar);
	mutex_init(&state.mutex);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&waiter, "selftest/condvar-timeout", kernel_selftest_condvar_timed_waiter_worker, &state),
		"failed to create timed condvar waiter thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, state.waiter_started, "waiter thread did not enter condvar_timed_wait", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiter), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_thread == &waiter->thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter->thread.block_reason == THREAD_BLOCK_WAIT_QUEUE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);

	timeout_deadline = state.waiter_start_tick + timeout_ticks;
	kernel_selftest_advance_ticks_until(timeout_deadline);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick >= timeout_deadline, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick - state.waiter_start_tick >= timeout_ticks, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex_after_wait, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(timeout_deadline);
	if (kernel_selftest_thread_is_live(waiter) && mutex_try_lock(&state.mutex)) {
		(void)condvar_signal(&state.condvar);
		(void)mutex_unlock(&state.mutex);
	}
	if (kernel_selftest_thread_is_live(waiter)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && waiter != NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter->thread));
	}
	kernel_selftest_thread_destroy(&waiter);
	kernel_selftest_clock_scope_end(&clock);
}

struct kernel_selftest_condvar_broadcast_state;

struct kernel_selftest_condvar_broadcast_arg {
	struct kernel_selftest_condvar_broadcast_state* state;
	size_t                                          index;
};

struct kernel_selftest_condvar_broadcast_state {
	struct condvar                               condvar;
	struct mutex                                 mutex;
	struct thread*                               waiter_threads[2];
	bool                                         waiter_started[2];
	bool                                         waiter_owned_mutex_before_wait[2];
	bool                                         waiter_woke[2];
	bool                                         waiter_owned_mutex_after_wait[2];
	bool                                         waiter_unlocked[2];
	struct kernel_selftest_condvar_broadcast_arg waiter_args[2];
};

static void kernel_selftest_condvar_broadcast_waiter_worker(void* arg) {
	struct kernel_selftest_condvar_broadcast_arg*   waiter_arg = arg;
	struct kernel_selftest_condvar_broadcast_state* state;
	size_t                                          index;

	if (waiter_arg == NULL || waiter_arg->state == NULL || waiter_arg->index >= 2u) return;

	state = waiter_arg->state;
	index = waiter_arg->index;

	mutex_lock(&state->mutex);
	state->waiter_threads[index]                 = kthread_current();
	state->waiter_started[index]                 = true;
	state->waiter_owned_mutex_before_wait[index] = mutex_is_owned_by_current(&state->mutex);
	condvar_wait(&state->condvar, &state->mutex);
	state->waiter_woke[index]                   = true;
	state->waiter_owned_mutex_after_wait[index] = mutex_is_owned_by_current(&state->mutex);
	state->waiter_unlocked[index]               = mutex_unlock(&state->mutex);
}

static void kernel_selftest_condvar_broadcast_wakes_all_waiters(struct kernel_selftest_context* ctx) {
	struct kthread*                                waiters[2] = {0};
	struct kernel_selftest_condvar_broadcast_state state      = {0};
	size_t                                         wake_count = 0u;

	condvar_init(&state.condvar);
	mutex_init(&state.mutex);
	for (size_t i = 0; i < 2u; i++) {
		state.waiter_args[i] = (struct kernel_selftest_condvar_broadcast_arg){
			.state = &state,
			.index = i,
		};
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_thread_create(&waiters[i],
		                                  i == 0u ? "selftest/condvar-broadcast-a" : "selftest/condvar-broadcast-b",
		                                  kernel_selftest_condvar_broadcast_waiter_worker,
		                                  &state.waiter_args[i]),
			"failed to create broadcast waiter thread",
			cleanup);
	}

	for (size_t i = 0; i < 2u; i++) {

		sched_yield();
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_started[i], cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex_before_wait[i], cleanup);
	}

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiters[0]), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiters[1]), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[0]->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[1]->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[0]->thread.block_reason == THREAD_BLOCK_WAIT_QUEUE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[1]->thread.block_reason == THREAD_BLOCK_WAIT_QUEUE, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, mutex_try_lock(&state.mutex), "main thread could not take mutex before broadcast", cleanup);

	wake_count = condvar_broadcast(&state.condvar);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_unlock(&state.mutex), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, wake_count == 2u, cleanup);

	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_woke, 2u) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, kernel_selftest_count_true(state.waiter_owned_mutex_after_wait, 2u) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_unlocked, 2u) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiters[0]->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiters[1]->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);

cleanup:
	if (mutex_try_lock(&state.mutex)) {
		(void)condvar_broadcast(&state.condvar);
		(void)mutex_unlock(&state.mutex);
	}
	if (kernel_selftest_thread_is_live(waiters[0]) || kernel_selftest_thread_is_live(waiters[1])) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL) {
		for (size_t i = 0; i < 2u; i++) {
			if (waiters[i] != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiters[i]->thread));
		}
	}
	for (size_t i = 0; i < 2u; i++) {
		kernel_selftest_thread_destroy(&waiters[i]);
	}
}

struct kernel_selftest_condvar_single_signal_state;

struct kernel_selftest_condvar_single_signal_arg {
	struct kernel_selftest_condvar_single_signal_state* state;
	size_t                                              index;
};

struct kernel_selftest_condvar_single_signal_state {
	struct condvar                                   condvar;
	struct mutex                                     mutex;
	struct thread*                                   waiter_threads[2];
	bool                                             waiter_started[2];
	bool                                             waiter_woke[2];
	bool                                             waiter_owned_mutex_after_wait[2];
	bool                                             waiter_unlocked[2];
	struct kernel_selftest_condvar_single_signal_arg waiter_args[2];
};

static void kernel_selftest_condvar_single_signal_waiter_worker(void* arg) {
	struct kernel_selftest_condvar_single_signal_arg*   waiter_arg = arg;
	struct kernel_selftest_condvar_single_signal_state* state;
	size_t                                              index;

	if (waiter_arg == NULL || waiter_arg->state == NULL || waiter_arg->index >= 2u) return;

	state = waiter_arg->state;
	index = waiter_arg->index;

	mutex_lock(&state->mutex);
	state->waiter_threads[index] = kthread_current();
	state->waiter_started[index] = true;
	condvar_wait(&state->condvar, &state->mutex);
	state->waiter_woke[index]                   = true;
	state->waiter_owned_mutex_after_wait[index] = mutex_is_owned_by_current(&state->mutex);
	state->waiter_unlocked[index]               = mutex_unlock(&state->mutex);
}

static void kernel_selftest_condvar_signal_wakes_single_waiter(struct kernel_selftest_context* ctx) {
	struct kthread*                                    waiters[2]    = {0};
	struct kernel_selftest_condvar_single_signal_state state         = {0};
	size_t                                             woken_index   = 0u;
	size_t                                             blocked_index = 0u;

	condvar_init(&state.condvar);
	mutex_init(&state.mutex);
	for (size_t i = 0; i < 2u; i++) {
		state.waiter_args[i] = (struct kernel_selftest_condvar_single_signal_arg){
			.state = &state,
			.index = i,
		};
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_thread_create(&waiters[i],
		                                  i == 0u ? "selftest/condvar-signal-a" : "selftest/condvar-signal-b",
		                                  kernel_selftest_condvar_single_signal_waiter_worker,
		                                  &state.waiter_args[i]),
			"failed to create condvar signal waiter thread",
			cleanup);
	}

	for (size_t i = 0; i < 2u; i++) {

		sched_yield();
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_started[i], cleanup);
	}

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, mutex_try_lock(&state.mutex), "main thread could not take mutex before signaling", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, condvar_signal(&state.condvar), "condvar_signal returned false with two blocked waiters", unlock);

unlock:
	if (mutex_is_owned_by_current(&state.mutex)) KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_unlock(&state.mutex), cleanup);

	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_woke, 2u) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, kernel_selftest_count_true(state.waiter_owned_mutex_after_wait, 2u) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_unlocked, 2u) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_wait_queue_depth(&state.condvar.waiters) == 1u, cleanup);

	woken_index   = state.waiter_woke[0] ? 0u : 1u;
	blocked_index = woken_index == 0u ? 1u : 0u;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiters[woken_index]->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(waiters[blocked_index]), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[blocked_index]->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[blocked_index]->thread.block_reason == THREAD_BLOCK_WAIT_QUEUE, cleanup);

cleanup:
	if (mutex_try_lock(&state.mutex)) {
		(void)condvar_broadcast(&state.condvar);
		(void)mutex_unlock(&state.mutex);
	}
	if (kernel_selftest_thread_is_live(waiters[0]) || kernel_selftest_thread_is_live(waiters[1])) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL) {
		for (size_t i = 0; i < 2u; i++) {
			if (waiters[i] != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiters[i]->thread));
		}
	}
	for (size_t i = 0; i < 2u; i++) {
		kernel_selftest_thread_destroy(&waiters[i]);
	}
}

static const struct kernel_selftest_case kernel_condvar_selftests[] = {
	{
     .name = "empty_queue_signal_and_broadcast_are_noops",
     .run  = kernel_selftest_condvar_empty_queue_signal_and_broadcast_are_noops,
	 },
	{
     .name = "wait_releases_mutex_and_reacquires_after_signal",
     .run  = kernel_selftest_condvar_wait_releases_mutex_and_reacquires_after_signal,
	 },
	{
     .name = "timed_wait_times_out_and_reacquires_mutex",
     .run  = kernel_selftest_condvar_timed_wait_times_out_and_reacquires_mutex,
	 },
	{
     .name = "broadcast_wakes_all_waiters",
     .run  = kernel_selftest_condvar_broadcast_wakes_all_waiters,
	 },
	{
     .name = "signal_wakes_single_waiter",
     .run  = kernel_selftest_condvar_signal_wakes_single_waiter,
	 },
};

const struct kernel_selftest_suite kernel_condvar_selftest_suite = {
	.name       = "condvar",
	.cases      = kernel_condvar_selftests,
	.case_count = sizeof(kernel_condvar_selftests) / sizeof(kernel_condvar_selftests[0]),
};
