#pragma once

#include <base/time.h>
#include <core/cpu.h>
#include <core/kthread.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static bool kernel_selftest_thread_create_with_preferred_cpu(struct kernel_selftest_managed_thread* managed,
                                                             const char* name, thread_entry_t entry, void* arg,
                                                             struct cpu* preferred_cpu) {
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

	if (!vmm_alloc(&stack_params, &stack_id, &stack_base)) return false;

	params = (struct thread_create_params){
		.name              = name,
		.entry             = entry,
		.arg               = arg,
		.kernel_stack_base = (uintptr_t)stack_base,
		.kernel_stack_top  = (uintptr_t)stack_base + KERNEL_SELFTEST_THREAD_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE,
		.preferred_cpu     = preferred_cpu,
		.detached          = false,
	};

	if (!kthread_create(&managed->thread, &params)) {
		(void)vmm_free(stack_id);
		return false;
	}

	managed->stack_id = stack_id;
	return true;
}

static bool kernel_selftest_thread_create(struct kernel_selftest_managed_thread* managed, const char* name,
                                          thread_entry_t entry, void* arg) {
	struct cpu* cpu = cpu_current();

	if (cpu == NULL) return false;

	return kernel_selftest_thread_create_with_preferred_cpu(managed, name, entry, arg, cpu);
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

static __attribute__((unused))
size_t kernel_selftest_count_true(const bool* values, size_t count) {
	size_t total = 0u;

	if (values == NULL) return 0u;

	for (size_t i = 0; i < count; i++) {
		if (values[i]) total++;
	}

	return total;
}

static uint64_t kernel_selftest_ms_to_ticks(uint64_t ms, uint32_t hz) {
	uint64_t ticks = 0u;

	if (!time_ms_to_ticks(ms, hz, &ticks)) return 0u;
	return ticks;
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
