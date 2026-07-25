#pragma once

#include <base/time.h>
#include <core/cpu.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KERNEL_SELFTEST_SLEEP_TICKS 3u
#define KERNEL_SELFTEST_MUTEX_HOLD_TICKS 2u
#define KERNEL_SELFTEST_MUTEX_TIMEOUT_MS 20u
#define KERNEL_SELFTEST_KTHREAD_SLEEP_MS 25u
#define KERNEL_SELFTEST_CLOCK_HZ 100u
#define KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS 8u

static __attribute__((unused))
bool kernel_selftest_thread_create_with_preferred_cpu(struct kthread** out_thread,
                                                                                     const char*      name,
                                                                                     thread_entry_t entry, void* arg,
                                                                                     struct cpu* preferred_cpu) {
	if (out_thread == NULL || name == NULL || entry == NULL) return false;

	*out_thread = NULL;
	return kthread_spawn_on_cpu(out_thread, name, entry, arg, preferred_cpu) == KTHREAD_SPAWN_OK;
}

static __attribute__((unused))
bool kernel_selftest_thread_create(struct kthread** out_thread, const char* name,
                                                                  thread_entry_t entry, void* arg) {
	struct cpu* cpu = cpu_current();

	if (cpu == NULL) return false;

	return kernel_selftest_thread_create_with_preferred_cpu(out_thread, name, entry, arg, cpu);
}

static __attribute__((unused))
void kernel_selftest_thread_destroy(struct kthread** thread) {
	if (thread == NULL || *thread == NULL) return;

	while ((*thread)->thread.state == THREAD_STATE_EXITING) {
		sched_yield();
	}

	if (!thread_is_reap_safe(&(*thread)->thread) && (*thread)->thread.state != THREAD_STATE_NEW) return;
	if (kthread_destroy(*thread)) *thread = NULL;
}

static __attribute__((unused))
bool kernel_selftest_thread_is_live(struct kthread* thread) {
	return thread != NULL && !thread_is_terminated(&thread->thread);
}

static __attribute__((unused))
void kernel_selftest_dispatch_rounds(size_t rounds) {
	for (size_t i = 0; i < rounds; i++) {
		sched_yield();
	}
}

static __attribute__((unused))
void kernel_selftest_advance_ticks_until(uint64_t deadline_tick) {
	while (sched_tick_count() < deadline_tick) {
		sched_tick();
	}
}

static __attribute__((unused))
size_t kernel_selftest_count_true(const bool* values, size_t count) {
	size_t total = 0u;

	if (values == NULL) return 0u;

	for (size_t i = 0; i < count; i++) {
		if (values[i]) total++;
	}

	return total;
}

static __attribute__((unused))
uint64_t kernel_selftest_ms_to_ticks(uint64_t ms, uint32_t hz) {
	uint64_t ticks = 0u;

	if (!time_ms_to_ticks(ms, hz, &ticks)) return 0u;
	return ticks;
}

struct kernel_selftest_clock_scope {
	uint32_t hz;
	bool     started;
};

static __attribute__((unused))
void kernel_selftest_clock_noop_handler(void* ctx) {
	(void)ctx;
}

static __attribute__((unused))
bool kernel_selftest_clock_scope_begin(struct kernel_selftest_clock_scope* scope) {
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

static __attribute__((unused))
void kernel_selftest_clock_scope_end(struct kernel_selftest_clock_scope* scope) {
	if (scope == NULL || !scope->started) return;

	hal_clock_stop();
	scope->started = false;
	scope->hz      = 0u;
}
