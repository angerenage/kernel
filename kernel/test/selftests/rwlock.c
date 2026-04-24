#include <core/rwlock.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"
#include "sync_helpers.h"

#define KERNEL_SELFTEST_RWLOCK_TIMEOUT_MS 20u
#define KERNEL_SELFTEST_RWLOCK_HOLD_TICKS 2u

struct kernel_selftest_rwlock_writer_priority_state {
	struct rwlock  rwlock;
	struct thread* holder_thread;
	struct thread* writer_thread;
	struct thread* reader_thread;
	uint64_t       holder_deadline_tick;
	uint64_t       writer_deadline_tick;
	bool           holder_acquired;
	bool           holder_sleep_result;
	bool           holder_unlocked;
	bool           writer_started;
	bool           writer_acquired;
	bool           writer_sleep_result;
	bool           writer_unlocked;
	bool           reader_started;
	bool           reader_try_result;
	bool           reader_acquired;
	bool           reader_unlocked;
};

static void kernel_selftest_rwlock_reader_holder_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_priority_state* state = arg;

	if (state == NULL) return;

	rwlock_read_lock(&state->rwlock);
	state->holder_thread        = kthread_current();
	state->holder_acquired      = true;
	state->holder_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->holder_sleep_result  = sched_sleep_until_tick(state->holder_deadline_tick);
	state->holder_unlocked      = rwlock_read_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_writer_waiter_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_priority_state* state = arg;

	if (state == NULL) return;

	state->writer_thread  = kthread_current();
	state->writer_started = true;
	rwlock_write_lock(&state->rwlock);
	state->writer_acquired      = true;
	state->writer_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->writer_sleep_result  = sched_sleep_until_tick(state->writer_deadline_tick);
	state->writer_unlocked      = rwlock_write_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_late_reader_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_priority_state* state = arg;

	if (state == NULL) return;

	state->reader_thread     = kthread_current();
	state->reader_started    = true;
	state->reader_try_result = rwlock_try_read_lock(&state->rwlock);
	if (!state->reader_try_result) rwlock_read_lock(&state->rwlock);
	state->reader_acquired = true;
	state->reader_unlocked = rwlock_read_unlock(&state->rwlock);
}

static void
kernel_selftest_rwlock_last_reader_wakes_writer_and_blocks_new_readers(struct kernel_selftest_context* ctx) {
	struct kthread*                                     holder = NULL;
	struct kthread*                                     writer = NULL;
	struct kthread*                                     reader = NULL;
	struct kernel_selftest_rwlock_writer_priority_state state  = {0};

	rwlock_init(&state.rwlock);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&holder, "selftest/rwlock-reader-holder", kernel_selftest_rwlock_reader_holder_worker, &state),
		"failed to create rwlock holder thread",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.holder_acquired, "holder thread never acquired the read lock", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&writer, "selftest/rwlock-writer", kernel_selftest_rwlock_writer_waiter_worker, &state),
		"failed to create rwlock writer thread",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.writer_started, "writer thread never attempted the write lock", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.writer_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 1u, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&reader, "selftest/rwlock-late-reader", kernel_selftest_rwlock_late_reader_worker, &state),
		"failed to create rwlock late reader thread",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.reader_started, "late reader never attempted the rwlock", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_try_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reader->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reader->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 2u, cleanup);

	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	kernel_selftest_dispatch_rounds(2u);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reader->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, reader->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&holder->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&writer->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&reader->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

cleanup:
	for (size_t attempt = 0; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS &&
	                         (kernel_selftest_thread_is_live(holder) || kernel_selftest_thread_is_live(writer) ||
	                          kernel_selftest_thread_is_live(reader));
	     attempt++) {
		kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
		kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
		sched_yield();
	}
	if (ctx->failure_expr == NULL) {
		if (holder != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&holder->thread));
		if (writer != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&writer->thread));
		if (reader != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&reader->thread));
	}
	kernel_selftest_thread_destroy(&reader);
	kernel_selftest_thread_destroy(&writer);
	kernel_selftest_thread_destroy(&holder);
}

struct kernel_selftest_rwlock_reader_broadcast_state;

struct kernel_selftest_rwlock_reader_broadcast_arg {
	struct kernel_selftest_rwlock_reader_broadcast_state* state;
	size_t                                                index;
};

struct kernel_selftest_rwlock_reader_broadcast_state {
	struct rwlock                                      rwlock;
	struct thread*                                     writer_thread;
	struct thread*                                     reader_threads[2];
	uint64_t                                           writer_deadline_tick;
	uint64_t                                           reader_deadline_ticks[2];
	bool                                               writer_locked;
	bool                                               writer_sleep_result;
	bool                                               writer_unlocked;
	bool                                               reader_started[2];
	bool                                               reader_acquired[2];
	bool                                               reader_sleep_result[2];
	bool                                               reader_unlocked[2];
	struct kernel_selftest_rwlock_reader_broadcast_arg reader_args[2];
};

static void kernel_selftest_rwlock_writer_holder_worker(void* arg) {
	struct kernel_selftest_rwlock_reader_broadcast_state* state = arg;

	if (state == NULL) return;

	rwlock_write_lock(&state->rwlock);
	state->writer_thread        = kthread_current();
	state->writer_locked        = true;
	state->writer_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->writer_sleep_result  = sched_sleep_until_tick(state->writer_deadline_tick);
	state->writer_unlocked      = rwlock_write_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_reader_broadcast_worker(void* arg) {
	struct kernel_selftest_rwlock_reader_broadcast_arg*   reader_arg = arg;
	struct kernel_selftest_rwlock_reader_broadcast_state* state;
	size_t                                                index;

	if (reader_arg == NULL || reader_arg->state == NULL || reader_arg->index >= 2u) return;

	state = reader_arg->state;
	index = reader_arg->index;

	state->reader_threads[index] = kthread_current();
	state->reader_started[index] = true;
	rwlock_read_lock(&state->rwlock);
	state->reader_acquired[index]       = true;
	state->reader_deadline_ticks[index] = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->reader_sleep_result[index]   = sched_sleep_until_tick(state->reader_deadline_ticks[index]);
	state->reader_unlocked[index]       = rwlock_read_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_writer_unlock_wakes_all_readers(struct kernel_selftest_context* ctx) {
	struct kthread*                                      writer     = NULL;
	struct kthread*                                      readers[2] = {0};
	struct kernel_selftest_rwlock_reader_broadcast_state state      = {0};

	rwlock_init(&state.rwlock);
	for (size_t i = 0; i < 2u; i++) {
		state.reader_args[i] = (struct kernel_selftest_rwlock_reader_broadcast_arg){
			.state = &state,
			.index = i,
		};
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&writer, "selftest/rwlock-writer-holder", kernel_selftest_rwlock_writer_holder_worker, &state),
		"failed to create rwlock writer holder",
		cleanup);
	for (size_t i = 0; i < 2u; i++) {
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_thread_create(&readers[i],
		                                  i == 0u ? "selftest/rwlock-reader-a" : "selftest/rwlock-reader-b",
		                                  kernel_selftest_rwlock_reader_broadcast_worker,
		                                  &state.reader_args[i]),
			"failed to create rwlock reader waiter",
			cleanup);
	}

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.writer_locked, "writer holder never acquired the rwlock", cleanup);

	for (size_t i = 0; i < 2u; i++) {
	}
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_started[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_started[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, readers[0]->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, readers[1]->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, readers[0]->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, readers[1]->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
	kernel_selftest_dispatch_rounds(2u);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.reader_acquired, 2u) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 0u, cleanup);

	for (size_t i = 0; i < 2u; i++) {
		kernel_selftest_advance_ticks_until(state.reader_deadline_ticks[i]);
	}
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_sleep_result[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_sleep_result[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_unlocked[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_unlocked[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&writer->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&readers[0]->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&readers[1]->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

cleanup:
	for (size_t attempt = 0; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS &&
	                         (kernel_selftest_thread_is_live(writer) || kernel_selftest_thread_is_live(readers[0]) ||
	                          kernel_selftest_thread_is_live(readers[1]));
	     attempt++) {
		kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
		for (size_t i = 0; i < 2u; i++) {
			kernel_selftest_advance_ticks_until(state.reader_deadline_ticks[i]);
		}
		sched_yield();
	}
	if (ctx->failure_expr == NULL) {
		if (writer != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&writer->thread));
		for (size_t i = 0; i < 2u; i++) {
			if (readers[i] != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&readers[i]->thread));
		}
	}
	for (size_t i = 0; i < 2u; i++) {
		kernel_selftest_thread_destroy(&readers[i]);
	}
	kernel_selftest_thread_destroy(&writer);
}

struct kernel_selftest_rwlock_writer_timeout_state {
	struct rwlock  rwlock;
	struct thread* holder_thread;
	struct thread* writer_thread;
	struct thread* reader_thread;
	uint64_t       holder_hold_ticks;
	uint64_t       holder_deadline_tick;
	uint64_t       writer_start_tick;
	uint64_t       writer_finish_tick;
	uint64_t       reader_deadline_tick;
	bool           holder_acquired;
	bool           holder_sleep_result;
	bool           holder_unlocked;
	bool           writer_started;
	bool           writer_finished;
	bool           writer_result;
	bool           reader_started;
	bool           reader_try_result;
	bool           reader_acquired;
	bool           reader_sleep_result;
	bool           reader_unlocked;
};

static void kernel_selftest_rwlock_timeout_holder_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_timeout_state* state = arg;

	if (state == NULL) return;

	rwlock_read_lock(&state->rwlock);
	state->holder_thread        = kthread_current();
	state->holder_acquired      = true;
	state->holder_deadline_tick = sched_tick_count() + state->holder_hold_ticks;
	state->holder_sleep_result  = sched_sleep_until_tick(state->holder_deadline_tick);
	state->holder_unlocked      = rwlock_read_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_timeout_writer_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_timeout_state* state = arg;

	if (state == NULL) return;

	state->writer_thread      = kthread_current();
	state->writer_started     = true;
	state->writer_start_tick  = sched_tick_count();
	state->writer_result      = rwlock_timed_write_lock(&state->rwlock, KERNEL_SELFTEST_RWLOCK_TIMEOUT_MS);
	state->writer_finish_tick = sched_tick_count();
	state->writer_finished    = true;
	if (state->writer_result) (void)rwlock_write_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_timeout_reader_worker(void* arg) {
	struct kernel_selftest_rwlock_writer_timeout_state* state = arg;

	if (state == NULL) return;

	state->reader_thread     = kthread_current();
	state->reader_started    = true;
	state->reader_try_result = rwlock_try_read_lock(&state->rwlock);
	if (!state->reader_try_result) rwlock_read_lock(&state->rwlock);
	state->reader_acquired      = true;
	state->reader_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->reader_sleep_result  = sched_sleep_until_tick(state->reader_deadline_tick);
	state->reader_unlocked      = rwlock_read_unlock(&state->rwlock);
}

static bool kernel_selftest_rwlock_wait_for_timeout_waiters(struct kernel_selftest_rwlock_writer_timeout_state* state,
                                                            size_t expected_waiters) {
	for (size_t attempt = 0; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS; attempt++) {
		if (state != NULL && !state->writer_finished && rwlock_waiter_count(&state->rwlock) == expected_waiters) {
			return true;
		}
		sched_yield();
	}
	return state != NULL && !state->writer_finished && rwlock_waiter_count(&state->rwlock) == expected_waiters;
}

static void kernel_selftest_rwlock_timed_writer_timeout_wakes_blocked_readers(struct kernel_selftest_context* ctx) {
	struct kthread*                                    holder           = NULL;
	struct kthread*                                    writer           = NULL;
	struct kthread*                                    reader           = NULL;
	struct kernel_selftest_rwlock_writer_timeout_state state            = {0};
	struct kernel_selftest_clock_scope                 clock            = {0};
	uint64_t                                           timeout_ticks    = 0u;
	uint64_t                                           timeout_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	timeout_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_RWLOCK_TIMEOUT_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, timeout_ticks != 0u, "timeout conversion returned zero ticks", cleanup);
	state.holder_hold_ticks = timeout_ticks + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;

	rwlock_init(&state.rwlock);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&holder, "selftest/rwlock-timeout-holder", kernel_selftest_rwlock_timeout_holder_worker, &state),
		"failed to create rwlock timeout holder",
		cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.holder_acquired, "timeout holder never acquired the read lock", cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&writer, "selftest/rwlock-timeout-writer", kernel_selftest_rwlock_timeout_writer_worker, &state),
		"failed to create rwlock timeout writer",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.writer_started, "timeout writer never attempted the rwlock", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.writer_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_rwlock_wait_for_timeout_waiters(&state, 1u), cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&reader, "selftest/rwlock-timeout-reader", kernel_selftest_rwlock_timeout_reader_worker, &state),
		"failed to create rwlock timeout reader",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.reader_started, "timeout reader never attempted the rwlock", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_try_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_rwlock_wait_for_timeout_waiters(&state, 2u), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 1u, cleanup);

	timeout_deadline = state.writer_start_tick + timeout_ticks;
	kernel_selftest_advance_ticks_until(timeout_deadline);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.writer_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_finish_tick >= timeout_deadline, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_thread_is_live(holder), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 2u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.reader_deadline_tick);
	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&holder->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&writer->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&reader->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(state.reader_deadline_tick);
	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	if (kernel_selftest_thread_is_live(holder) || kernel_selftest_thread_is_live(writer) ||
	    kernel_selftest_thread_is_live(reader)) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL) {
		if (holder != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&holder->thread));
		if (writer != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&writer->thread));
		if (reader != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&reader->thread));
	}
	kernel_selftest_thread_destroy(&reader);
	kernel_selftest_thread_destroy(&writer);
	kernel_selftest_thread_destroy(&holder);
	kernel_selftest_clock_scope_end(&clock);
}

struct kernel_selftest_rwlock_downgrade_state {
	struct rwlock  rwlock;
	struct thread* downgrader_thread;
	struct thread* writer_thread;
	struct thread* reader_thread;
	uint64_t       downgrade_deadline_tick;
	uint64_t       read_deadline_tick;
	uint64_t       writer_deadline_tick;
	uint64_t       reader_deadline_tick;
	bool           downgrader_acquired;
	bool           downgrader_write_sleep_result;
	bool           downgrader_downgraded;
	bool           downgrader_read_sleep_result;
	bool           downgrader_unlocked;
	bool           writer_started;
	bool           writer_acquired;
	bool           writer_sleep_result;
	bool           writer_unlocked;
	bool           reader_started;
	bool           reader_try_result;
	bool           reader_acquired;
	bool           reader_sleep_result;
	bool           reader_unlocked;
};

static void kernel_selftest_rwlock_downgrader_worker(void* arg) {
	struct kernel_selftest_rwlock_downgrade_state* state = arg;

	if (state == NULL) return;

	rwlock_write_lock(&state->rwlock);
	state->downgrader_thread             = kthread_current();
	state->downgrader_acquired           = true;
	state->downgrade_deadline_tick       = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->downgrader_write_sleep_result = sched_sleep_until_tick(state->downgrade_deadline_tick);
	state->downgrader_downgraded         = rwlock_downgrade(&state->rwlock);
	state->read_deadline_tick            = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->downgrader_read_sleep_result  = sched_sleep_until_tick(state->read_deadline_tick);
	state->downgrader_unlocked           = rwlock_read_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_downgrade_writer_worker(void* arg) {
	struct kernel_selftest_rwlock_downgrade_state* state = arg;

	if (state == NULL) return;

	state->writer_thread  = kthread_current();
	state->writer_started = true;
	rwlock_write_lock(&state->rwlock);
	state->writer_acquired      = true;
	state->writer_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->writer_sleep_result  = sched_sleep_until_tick(state->writer_deadline_tick);
	state->writer_unlocked      = rwlock_write_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_downgrade_reader_worker(void* arg) {
	struct kernel_selftest_rwlock_downgrade_state* state = arg;

	if (state == NULL) return;

	state->reader_thread     = kthread_current();
	state->reader_started    = true;
	state->reader_try_result = rwlock_try_read_lock(&state->rwlock);
	if (!state->reader_try_result) rwlock_read_lock(&state->rwlock);
	state->reader_acquired      = true;
	state->reader_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_RWLOCK_HOLD_TICKS;
	state->reader_sleep_result  = sched_sleep_until_tick(state->reader_deadline_tick);
	state->reader_unlocked      = rwlock_read_unlock(&state->rwlock);
}

static void kernel_selftest_rwlock_downgrade_preserves_waiting_writer_priority(struct kernel_selftest_context* ctx) {
	struct kthread*                               downgrader = NULL;
	struct kthread*                               writer     = NULL;
	struct kthread*                               reader     = NULL;
	struct kernel_selftest_rwlock_downgrade_state state      = {0};

	rwlock_init(&state.rwlock);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&downgrader, "selftest/rwlock-downgrader", kernel_selftest_rwlock_downgrader_worker, &state),
		"failed to create rwlock downgrader",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, state.downgrader_acquired, "downgrader never acquired the write lock", cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&writer, "selftest/rwlock-downgrade-writer", kernel_selftest_rwlock_downgrade_writer_worker, &state),
		"failed to create rwlock downgrade writer",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_started, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.writer_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, writer->thread.block_reason == THREAD_BLOCK_RWLOCK, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&reader, "selftest/rwlock-downgrade-reader", kernel_selftest_rwlock_downgrade_reader_worker, &state),
		"failed to create rwlock downgrade reader",
		cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_started, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_try_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 2u, cleanup);

	kernel_selftest_advance_ticks_until(state.downgrade_deadline_tick);
	kernel_selftest_dispatch_rounds(2u);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.downgrader_write_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.downgrader_downgraded, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.writer_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 2u, cleanup);

	kernel_selftest_advance_ticks_until(state.read_deadline_tick);
	kernel_selftest_dispatch_rounds(2u);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.downgrader_read_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.downgrader_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
	kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.writer_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.reader_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&downgrader->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&writer->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&reader->thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_reader_count(&state.rwlock) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, rwlock_waiter_count(&state.rwlock) == 0u, cleanup);

cleanup:
	for (size_t attempt = 0; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS &&
	                         (kernel_selftest_thread_is_live(downgrader) || kernel_selftest_thread_is_live(writer) ||
	                          kernel_selftest_thread_is_live(reader));
	     attempt++) {
		kernel_selftest_advance_ticks_until(state.downgrade_deadline_tick);
		kernel_selftest_advance_ticks_until(state.read_deadline_tick);
		kernel_selftest_advance_ticks_until(state.writer_deadline_tick);
		kernel_selftest_advance_ticks_until(state.reader_deadline_tick);
		sched_yield();
	}
	if (ctx->failure_expr == NULL) {
		if (downgrader != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&downgrader->thread));
		if (writer != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&writer->thread));
		if (reader != NULL) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&reader->thread));
	}
	kernel_selftest_thread_destroy(&reader);
	kernel_selftest_thread_destroy(&writer);
	kernel_selftest_thread_destroy(&downgrader);
}

static const struct kernel_selftest_case kernel_rwlock_selftests[] = {
	{
     .name = "last_reader_wakes_writer_and_blocks_new_readers",
     .run  = kernel_selftest_rwlock_last_reader_wakes_writer_and_blocks_new_readers,
	 },
	{
     .name = "writer_unlock_wakes_all_readers",
     .run  = kernel_selftest_rwlock_writer_unlock_wakes_all_readers,
	 },
	{
     .name = "timed_writer_timeout_wakes_blocked_readers",
     .run  = kernel_selftest_rwlock_timed_writer_timeout_wakes_blocked_readers,
	 },
	{
     .name = "downgrade_preserves_waiting_writer_priority",
     .run  = kernel_selftest_rwlock_downgrade_preserves_waiting_writer_priority,
	 },
};

const struct kernel_selftest_suite kernel_rwlock_selftest_suite = {
	.name       = "rwlock",
	.cases      = kernel_rwlock_selftests,
	.case_count = sizeof(kernel_rwlock_selftests) / sizeof(kernel_rwlock_selftests[0]),
};
