#include <core/cpu.h>
#include <core/kthread.h>
#include <core/mutex.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../selftest.h"

#define KERNEL_SELFTEST_THREAD_STACK_PAGES 4u
#define KERNEL_SELFTEST_SLEEP_TICKS 3u
#define KERNEL_SELFTEST_MUTEX_HOLD_TICKS 2u
#define KERNEL_SELFTEST_MUTEX_TIMEOUT_MS 20u
#define KERNEL_SELFTEST_KTHREAD_SLEEP_MS 25u
#define KERNEL_SELFTEST_CLOCK_HZ 100u
#define KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS 8u

struct kernel_selftest_managed_thread {
	struct thread thread;
	vmm_id_t      stack_id;
};

static bool kernel_selftest_thread_create(struct kernel_selftest_managed_thread* managed, const char* name,
                                          thread_entry_t entry, void* arg) {
	struct cpu*                 cpu;
	struct thread_create_params params;
	struct vmm_alloc_params     stack_params = {
			.page_count  = KERNEL_SELFTEST_THREAD_STACK_PAGES,
			.align_pages = 1u,
			.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
			.kind        = VMM_KIND_STACK,
			.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
			.map_flags   = 0u,
    };
	void*    stack_base = NULL;
	vmm_id_t stack_id   = VMM_ID_INVALID;

	if (managed == NULL || name == NULL || entry == NULL) return false;

	*managed = (struct kernel_selftest_managed_thread){
		.stack_id = VMM_ID_INVALID,
	};

	cpu = cpu_current();
	if (cpu == NULL) return false;
	if (!vmm_alloc(&stack_params, &stack_id, &stack_base)) return false;

	params = (struct thread_create_params){
		.name              = name,
		.entry             = entry,
		.arg               = arg,
		.kernel_stack_base = (uintptr_t)stack_base,
		.kernel_stack_top  = (uintptr_t)stack_base + KERNEL_SELFTEST_THREAD_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE,
		.preferred_cpu     = cpu,
		.detached          = false,
	};

	if (!kthread_create(&managed->thread, &params)) {
		(void)vmm_free(stack_id);
		return false;
	}

	managed->stack_id = stack_id;
	return true;
}

static void kernel_selftest_thread_destroy(struct kernel_selftest_managed_thread* managed) {
	if (managed == NULL || managed->stack_id == VMM_ID_INVALID) return;
	if (!thread_is_terminated(&managed->thread) && managed->thread.state != THREAD_STATE_NEW) return;

	(void)vmm_free(managed->stack_id);
	managed->stack_id = VMM_ID_INVALID;
}

static void kernel_selftest_dispatch_rounds(size_t rounds) {
	for (size_t i = 0; i < rounds; i++) {
		sched_yield();
	}
}

static void kernel_selftest_advance_ticks_until(uint64_t deadline_tick) {
	while (sched_tick_count() < deadline_tick) {
		sched_tick();
	}
}

static size_t kernel_selftest_count_true(const bool* values, size_t count) {
	size_t total = 0u;

	if (values == NULL) return 0u;

	for (size_t i = 0; i < count; i++) {
		if (values[i]) total++;
	}

	return total;
}

static uint64_t kernel_selftest_ms_to_ticks(uint64_t ms, uint32_t hz) {
	uint64_t sleep_ticks;

	if (ms == 0u || hz == 0u) return 0u;

	if (ms > (UINT64_MAX - 999u) / (uint64_t)hz) {
		sleep_ticks = UINT64_MAX;
	}
	else {
		sleep_ticks = (ms * (uint64_t)hz + 999u) / 1000u;
	}

	return sleep_ticks == 0u ? 1u : sleep_ticks;
}

struct kernel_selftest_clock_scope {
	uint32_t hz;
	bool     started;
};

static void kernel_selftest_clock_noop_handler(void* ctx) {
	(void)ctx;
}

static bool kernel_selftest_clock_scope_begin(struct kernel_selftest_clock_scope* scope) {
	if (scope == NULL) return false;

	*scope = (struct kernel_selftest_clock_scope){0};
	hal_clock_init();
	scope->hz = hal_clock_frequency();
	if (scope->hz != 0u) return true;
	if (!hal_clock_start(KERNEL_SELFTEST_CLOCK_HZ, kernel_selftest_clock_noop_handler, NULL)) return false;

	scope->started = true;
	scope->hz      = hal_clock_frequency();
	return scope->hz != 0u;
}

static void kernel_selftest_clock_scope_end(struct kernel_selftest_clock_scope* scope) {
	if (scope == NULL || !scope->started) return;

	hal_clock_stop();
	scope->started = false;
	scope->hz      = 0u;
}

struct kernel_selftest_sched_dispatch_state {
	struct cpu*    cpu_at_entry;
	struct thread* current_thread;
	bool           ran;
};

static void kernel_selftest_sched_dispatch_worker(void* arg) {
	struct kernel_selftest_sched_dispatch_state* state = arg;

	if (state == NULL) return;

	state->cpu_at_entry   = cpu_current();
	state->current_thread = kthread_current();
	state->ran            = true;
}

static void kernel_selftest_sched_yield_dispatches_runnable_thread(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread worker = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_sched_dispatch_state state = {0};
	struct sched_stats                          stats_before;
	struct sched_stats                          stats_after;
	struct cpu*                                 cpu = cpu_current();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);

	sched_get_stats(&stats_before);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&worker, "selftest/sched-dispatch", kernel_selftest_sched_dispatch_worker, &state),
		"failed to create scheduler dispatch worker",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&worker.thread), "failed to start scheduler dispatch worker", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) == 1u, cleanup);

	sched_yield();

	sched_get_stats(&stats_after);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.ran, "worker thread was not dispatched", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.cpu_at_entry == cpu, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.current_thread == &worker.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.exit_code == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, stats_after.yield_count >= stats_before.yield_count + 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, stats_after.context_switch_count >= stats_before.context_switch_count + 2u, cleanup);

cleanup:
	if (!thread_is_terminated(&worker.thread)) kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	if (ctx->failure_expr == NULL && worker.stack_id != VMM_ID_INVALID) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker.thread));
	}
	kernel_selftest_thread_destroy(&worker);
}

struct kernel_selftest_sched_sleep_state {
	uint64_t deadline_tick;
	uint64_t wake_tick;
	bool     entered;
	bool     slept;
};

static void kernel_selftest_sched_sleep_worker(void* arg) {
	struct kernel_selftest_sched_sleep_state* state = arg;

	if (state == NULL) return;

	state->deadline_tick = sched_tick_count() + KERNEL_SELFTEST_SLEEP_TICKS;
	state->entered       = true;
	state->slept         = sched_sleep_until_tick(state->deadline_tick);
	state->wake_tick     = sched_tick_count();
}

static void kernel_selftest_sched_sleep_wakes_after_deadline(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread worker = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_sched_sleep_state state = {0};
	struct cpu*                              cpu   = cpu_current();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(&worker, "selftest/sched-sleep", kernel_selftest_sched_sleep_worker, &state),
		"failed to create scheduler sleep worker",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&worker.thread), "failed to start scheduler sleep worker", cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.entered, "worker thread did not reach sched_sleep_until_tick", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&worker.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);

	for (size_t i = 0; i + 1u < KERNEL_SELFTEST_SLEEP_TICKS; i++) {
		sched_tick();
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&worker.thread), cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) == 0u, cleanup);
	}

	sched_tick();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) >= 1u, cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.slept, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.wake_tick >= state.deadline_tick, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker.thread), cleanup);

cleanup:
	if (!thread_is_terminated(&worker.thread)) {
		kernel_selftest_advance_ticks_until(state.deadline_tick);
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL && worker.stack_id != VMM_ID_INVALID) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker.thread));
	}
	kernel_selftest_thread_destroy(&worker);
}

struct kernel_selftest_kthread_sleep_ms_state {
	uint64_t start_tick;
	uint64_t wake_tick;
	bool     started;
	bool     slept;
};

static void kernel_selftest_kthread_sleep_ms_worker(void* arg) {
	struct kernel_selftest_kthread_sleep_ms_state* state = arg;

	if (state == NULL) return;

	state->start_tick = sched_tick_count();
	state->started    = true;
	state->slept      = kthread_sleep_ms(KERNEL_SELFTEST_KTHREAD_SLEEP_MS);
	state->wake_tick  = sched_tick_count();
}

static void kernel_selftest_sleep_ms_wakes_after_deadline(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread worker = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_kthread_sleep_ms_state state = {0};
	struct kernel_selftest_clock_scope            clock = {0};
	struct cpu*                                   cpu   = cpu_current();
	uint64_t                                      sleep_ticks;
	uint64_t                                      deadline_tick = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);

	sleep_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_KTHREAD_SLEEP_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, sleep_ticks != 0u, "ms to ticks conversion returned zero", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&worker, "selftest/kthread-sleep-ms", kernel_selftest_kthread_sleep_ms_worker, &state),
		"failed to create kthread_sleep_ms worker",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&worker.thread), "failed to start kthread_sleep_ms worker", cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.started, "worker thread did not call kthread_sleep_ms", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&worker.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker.thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);

	deadline_tick = state.start_tick + sleep_ticks;
	for (uint64_t tick = 0; tick + 1u < sleep_ticks; tick++) {
		sched_tick();
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&worker.thread), cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) == 0u, cleanup);
	}

	sched_tick();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) >= 1u, cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.slept, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.wake_tick >= deadline_tick, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.wake_tick - state.start_tick >= sleep_ticks, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&worker.thread), cleanup);

cleanup:
	if (!thread_is_terminated(&worker.thread)) {
		kernel_selftest_advance_ticks_until(deadline_tick);
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL && worker.stack_id != VMM_ID_INVALID) {
		KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&worker.thread));
	}
	kernel_selftest_thread_destroy(&worker);
	kernel_selftest_clock_scope_end(&clock);
}

struct kernel_selftest_mutex_state {
	struct mutex   mutex;
	struct thread* holder_thread;
	struct thread* waiter_thread;
	uint64_t       holder_deadline_tick;
	uint64_t       holder_wake_tick;
	bool           holder_acquired;
	bool           holder_sleep_result;
	bool           holder_unlocked;
	bool           waiter_started;
	bool           waiter_acquired;
	bool           waiter_owned_mutex;
	bool           waiter_unlocked;
};

static void kernel_selftest_mutex_holder_worker(void* arg) {
	struct kernel_selftest_mutex_state* state = arg;

	if (state == NULL) return;

	mutex_lock(&state->mutex);
	state->holder_thread        = kthread_current();
	state->holder_acquired      = mutex_is_owned_by_current(&state->mutex);
	state->holder_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_MUTEX_HOLD_TICKS;
	state->holder_sleep_result  = sched_sleep_until_tick(state->holder_deadline_tick);
	state->holder_wake_tick     = sched_tick_count();
	state->holder_unlocked      = mutex_unlock(&state->mutex);
}

static void kernel_selftest_mutex_waiter_worker(void* arg) {
	struct kernel_selftest_mutex_state* state = arg;

	if (state == NULL) return;

	state->waiter_thread  = kthread_current();
	state->waiter_started = true;
	mutex_lock(&state->mutex);
	state->waiter_acquired    = true;
	state->waiter_owned_mutex = mutex_is_owned_by_current(&state->mutex);
	state->waiter_unlocked    = mutex_unlock(&state->mutex);
}

struct kernel_selftest_mutex_timeout_state {
	struct mutex   mutex;
	struct thread* owner_thread;
	struct thread* waiter_thread;
	uint64_t       owner_deadline_tick;
	uint64_t       owner_wake_tick;
	uint64_t       waiter_start_tick;
	uint64_t       waiter_finish_tick;
	bool           owner_locked;
	bool           owner_sleep_result;
	bool           owner_unlocked;
	bool           waiter_started;
	bool           waiter_result;
	bool           waiter_finished;
	bool           waiter_owned_mutex;
};

static void kernel_selftest_mutex_timeout_owner_worker(void* arg) {
	struct kernel_selftest_mutex_timeout_state* state = arg;

	if (state == NULL) return;

	mutex_lock(&state->mutex);
	state->owner_thread        = kthread_current();
	state->owner_locked        = mutex_is_owned_by_current(&state->mutex);
	state->owner_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_SLEEP_TICKS + KERNEL_SELFTEST_MUTEX_HOLD_TICKS;
	state->owner_sleep_result  = sched_sleep_until_tick(state->owner_deadline_tick);
	state->owner_wake_tick     = sched_tick_count();
	state->owner_unlocked      = mutex_unlock(&state->mutex);
}

static void kernel_selftest_mutex_timeout_waiter_worker(void* arg) {
	struct kernel_selftest_mutex_timeout_state* state = arg;

	if (state == NULL) return;

	state->waiter_thread      = kthread_current();
	state->waiter_started     = true;
	state->waiter_start_tick  = sched_tick_count();
	state->waiter_result      = mutex_timed_lock(&state->mutex, KERNEL_SELFTEST_MUTEX_TIMEOUT_MS);
	state->waiter_finish_tick = sched_tick_count();
	state->waiter_owned_mutex = mutex_is_owned_by_current(&state->mutex);
	state->waiter_finished    = true;
	if (state->waiter_result) (void)mutex_unlock(&state->mutex);
}

static void kernel_selftest_mutex_timed_lock_times_out_when_owner_never_unlocks(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread owner = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_managed_thread waiter = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_mutex_timeout_state state = {0};
	struct kernel_selftest_clock_scope         clock = {0};
	uint64_t                                   timeout_ticks;
	uint64_t                                   timeout_deadline = 0u;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);
	timeout_ticks = kernel_selftest_ms_to_ticks(KERNEL_SELFTEST_MUTEX_TIMEOUT_MS, clock.hz);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, timeout_ticks != 0u, "timeout conversion returned zero ticks", cleanup);

	mutex_init(&state.mutex);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&owner, "selftest/mutex-timeout-owner", kernel_selftest_mutex_timeout_owner_worker, &state),
		"failed to create mutex timeout owner thread",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&waiter, "selftest/mutex-timeout-waiter", kernel_selftest_mutex_timeout_waiter_worker, &state),
		"failed to create mutex timeout waiter thread",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, kthread_start(&owner.thread), "failed to start timeout owner thread", cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.owner_locked, "owner thread never acquired the mutex", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &owner.thread, cleanup);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&waiter.thread), "failed to start timeout waiter thread", cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.waiter_started, "timed waiter never attempted the mutex", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter.thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter.thread.block_reason == THREAD_BLOCK_MUTEX, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &owner.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 1u, cleanup);

	timeout_deadline = state.waiter_start_tick + timeout_ticks;
	kernel_selftest_advance_ticks_until(timeout_deadline);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finished, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_owned_mutex, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick >= timeout_deadline, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_finish_tick - state.waiter_start_tick >= timeout_ticks, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &owner.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&owner.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 0u, cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(state.owner_deadline_tick);
	if (!thread_is_terminated(&owner.thread) || !thread_is_terminated(&waiter.thread)) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL) {
		if (owner.stack_id != VMM_ID_INVALID) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&owner.thread));
		if (waiter.stack_id != VMM_ID_INVALID) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter.thread));
	}
	kernel_selftest_thread_destroy(&waiter);
	kernel_selftest_thread_destroy(&owner);
	kernel_selftest_clock_scope_end(&clock);
}

struct kernel_selftest_mutex_single_wake_state;

struct kernel_selftest_mutex_single_wake_waiter_arg {
	struct kernel_selftest_mutex_single_wake_state* state;
	size_t                                          index;
};

struct kernel_selftest_mutex_single_wake_state {
	struct mutex                                        mutex;
	struct thread*                                      holder_thread;
	struct thread*                                      waiter_threads[2];
	uint64_t                                            holder_deadline_tick;
	uint64_t                                            waiter_deadline_ticks[2];
	bool                                                holder_locked;
	bool                                                holder_sleep_result;
	bool                                                holder_unlocked;
	bool                                                waiter_started[2];
	bool                                                waiter_acquired[2];
	bool                                                waiter_owned_mutex[2];
	bool                                                waiter_sleep_result[2];
	bool                                                waiter_unlocked[2];
	struct kernel_selftest_mutex_single_wake_waiter_arg waiter_args[2];
};

static void kernel_selftest_mutex_single_wake_holder_worker(void* arg) {
	struct kernel_selftest_mutex_single_wake_state* state = arg;

	if (state == NULL) return;

	mutex_lock(&state->mutex);
	state->holder_thread        = kthread_current();
	state->holder_locked        = mutex_is_owned_by_current(&state->mutex);
	state->holder_deadline_tick = sched_tick_count() + KERNEL_SELFTEST_MUTEX_HOLD_TICKS;
	state->holder_sleep_result  = sched_sleep_until_tick(state->holder_deadline_tick);
	state->holder_unlocked      = mutex_unlock(&state->mutex);
}

static void kernel_selftest_mutex_single_wake_waiter_worker(void* arg) {
	struct kernel_selftest_mutex_single_wake_waiter_arg* waiter_arg = arg;
	struct kernel_selftest_mutex_single_wake_state*      state;
	size_t                                               index;

	if (waiter_arg == NULL || waiter_arg->state == NULL || waiter_arg->index >= 2u) return;

	state = waiter_arg->state;
	index = waiter_arg->index;

	state->waiter_threads[index] = kthread_current();
	state->waiter_started[index] = true;
	mutex_lock(&state->mutex);
	state->waiter_acquired[index]       = true;
	state->waiter_owned_mutex[index]    = mutex_is_owned_by_current(&state->mutex);
	state->waiter_deadline_ticks[index] = sched_tick_count() + KERNEL_SELFTEST_MUTEX_HOLD_TICKS;
	state->waiter_sleep_result[index]   = sched_sleep_until_tick(state->waiter_deadline_ticks[index]);
	state->waiter_unlocked[index]       = mutex_unlock(&state->mutex);
}

static void kernel_selftest_mutex_unlock_wakes_single_waiter(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread holder = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_managed_thread waiters[2] = {
		{.stack_id = VMM_ID_INVALID},
		{.stack_id = VMM_ID_INVALID},
	};
	struct kernel_selftest_mutex_single_wake_state state         = {0};
	size_t                                         woken_index   = 0u;
	size_t                                         blocked_index = 0u;

	mutex_init(&state.mutex);
	for (size_t i = 0; i < 2u; i++) {
		state.waiter_args[i] = (struct kernel_selftest_mutex_single_wake_waiter_arg){
			.state = &state,
			.index = i,
		};
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(
			&holder, "selftest/mutex-single-holder", kernel_selftest_mutex_single_wake_holder_worker, &state),
		"failed to create mutex single-wake holder thread",
		cleanup);
	for (size_t i = 0; i < 2u; i++) {
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_thread_create(&waiters[i],
		                                  i == 0u ? "selftest/mutex-single-waiter-b" : "selftest/mutex-single-waiter-c",
		                                  kernel_selftest_mutex_single_wake_waiter_worker,
		                                  &state.waiter_args[i]),
			"failed to create mutex single-wake waiter thread",
			cleanup);
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kthread_start(&holder.thread), "failed to start single-wake holder thread", cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.holder_locked, "holder thread never acquired the mutex", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &holder.thread, cleanup);

	for (size_t i = 0; i < 2u; i++) {
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, kthread_start(&waiters[i].thread), "failed to start single-wake waiter thread", cleanup);
	}
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_started[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_started[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[0].thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[1].thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[0].thread.block_reason == THREAD_BLOCK_MUTEX, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[1].thread.block_reason == THREAD_BLOCK_MUTEX, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 2u, cleanup);

	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&holder.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_acquired, 2u) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, kernel_selftest_count_true(state.waiter_owned_mutex, 2u) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 1u, cleanup);

	woken_index   = state.waiter_acquired[0] ? 0u : 1u;
	blocked_index = woken_index == 0u ? 1u : 0u;

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_acquired[woken_index], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex[woken_index], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !state.waiter_acquired[blocked_index], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &waiters[woken_index].thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[woken_index].thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[woken_index].thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[blocked_index].thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiters[blocked_index].thread.block_reason == THREAD_BLOCK_MUTEX, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 1u, cleanup);

	kernel_selftest_advance_ticks_until(state.waiter_deadline_ticks[woken_index]);
	sched_yield();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_acquired[blocked_index], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex[blocked_index], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &waiters[blocked_index].thread, cleanup);

	kernel_selftest_advance_ticks_until(state.waiter_deadline_ticks[blocked_index]);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_sleep_result[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_sleep_result[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_unlocked[0], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_unlocked[1], cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiters[0].thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiters[1].thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 0u, cleanup);

cleanup:
	for (size_t attempt = 0; attempt < KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS &&
	                         (!thread_is_terminated(&holder.thread) || !thread_is_terminated(&waiters[0].thread) ||
	                          !thread_is_terminated(&waiters[1].thread));
	     attempt++) {
		kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
		for (size_t i = 0; i < 2u; i++) {
			kernel_selftest_advance_ticks_until(state.waiter_deadline_ticks[i]);
		}
		sched_yield();
	}
	if (ctx->failure_expr == NULL) {
		if (holder.stack_id != VMM_ID_INVALID) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&holder.thread));
		for (size_t i = 0; i < 2u; i++) {
			if (waiters[i].stack_id != VMM_ID_INVALID)
				KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiters[i].thread));
		}
	}
	for (size_t i = 0; i < 2u; i++) {
		kernel_selftest_thread_destroy(&waiters[i]);
	}
	kernel_selftest_thread_destroy(&holder);
}

static void kernel_selftest_mutex_contention_blocks_and_wakes_waiter(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_managed_thread holder = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_managed_thread waiter = {
		.stack_id = VMM_ID_INVALID,
	};
	struct kernel_selftest_mutex_state state = {0};
	struct cpu*                        cpu   = cpu_current();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_current returned NULL", cleanup);

	mutex_init(&state.mutex);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(&holder, "selftest/mutex-holder", kernel_selftest_mutex_holder_worker, &state),
		"failed to create mutex holder thread",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx,
		kernel_selftest_thread_create(&waiter, "selftest/mutex-waiter", kernel_selftest_mutex_waiter_worker, &state),
		"failed to create mutex waiter thread",
		cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, kthread_start(&holder.thread), "failed to start mutex holder thread", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, kthread_start(&waiter.thread), "failed to start mutex waiter thread", cleanup);

	sched_yield();

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.holder_acquired, "holder thread never acquired the mutex", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, state.waiter_started, "waiter thread never attempted the mutex", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&holder.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !thread_is_terminated(&waiter.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, holder.thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, holder.thread.block_reason == THREAD_BLOCK_SLEEP, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter.thread.state == THREAD_STATE_BLOCKED, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, waiter.thread.block_reason == THREAD_BLOCK_MUTEX, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_thread == &holder.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_thread == &waiter.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == &holder.thread, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) == 0u, cleanup);

	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_run_queue_depth(cpu) >= 1u, cleanup);
	sched_yield();

	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_sleep_result, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_wake_tick >= state.holder_deadline_tick, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.holder_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_acquired, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_owned_mutex, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, state.waiter_unlocked, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&holder.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, thread_is_terminated(&waiter.thread), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !mutex_is_locked(&state.mutex), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_owner(&state.mutex) == NULL, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, mutex_waiter_count(&state.mutex) == 0u, cleanup);

cleanup:
	kernel_selftest_advance_ticks_until(state.holder_deadline_tick);
	if (!thread_is_terminated(&holder.thread) || !thread_is_terminated(&waiter.thread)) {
		kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
	}
	if (ctx->failure_expr == NULL) {
		if (holder.stack_id != VMM_ID_INVALID) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&holder.thread));
		if (waiter.stack_id != VMM_ID_INVALID) KERNEL_SELFTEST_ASSERT(ctx, thread_is_terminated(&waiter.thread));
	}
	kernel_selftest_thread_destroy(&waiter);
	kernel_selftest_thread_destroy(&holder);
}

static const struct kernel_selftest_case kernel_sched_mutex_selftests[] = {
	{
     .name = "yield_dispatches_runnable_thread",
     .run  = kernel_selftest_sched_yield_dispatches_runnable_thread,
	 },
	{
     .name = "sleep_wakes_after_deadline",
     .run  = kernel_selftest_sched_sleep_wakes_after_deadline,
	 },
	{
     .name = "mutex_contention_blocks_and_wakes_waiter",
     .run  = kernel_selftest_mutex_contention_blocks_and_wakes_waiter,
	 },
	{
     .name = "mutex_timed_lock_times_out_when_owner_never_unlocks",
     .run  = kernel_selftest_mutex_timed_lock_times_out_when_owner_never_unlocks,
	 },
	{
     .name = "mutex_unlock_wakes_single_waiter",
     .run  = kernel_selftest_mutex_unlock_wakes_single_waiter,
	 },
	{
     .name = "sleep_ms_wakes_after_deadline",
     .run  = kernel_selftest_sleep_ms_wakes_after_deadline,
	 },
};

const struct kernel_selftest_suite kernel_sched_mutex_selftest_suite = {
	.name       = "sched_mutex",
	.cases      = kernel_sched_mutex_selftests,
	.case_count = sizeof(kernel_sched_mutex_selftests) / sizeof(kernel_sched_mutex_selftests[0]),
};
