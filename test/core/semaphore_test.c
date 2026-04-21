#include <core/cpu.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <core/semaphore.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

static bool              semaphore_test_hook_active;
static size_t            semaphore_test_hook_runs;
static struct semaphore* semaphore_test_semaphore;
static struct thread*    semaphore_test_signaler;

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

static void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_mock_set_context_switch_hook(NULL);
	semaphore_test_hook_active = false;
	semaphore_test_hook_runs   = 0u;
	semaphore_test_semaphore   = NULL;
	semaphore_test_signaler    = NULL;
	hal_cpu_local_bind(NULL);
}

static void semaphore_test_entry(void* arg) {
	(void)arg;
}

static void semaphore_test_set_one_tick_timeslice(struct thread* thread) {
	if (thread == NULL) return;

	thread->timeslice_ticks     = 1u;
	thread->timeslice_remaining = 1u;
}

static void semaphore_test_signal_context_switch_hook(struct thread_context*       current,
                                                      const struct thread_context* next) {
	(void)current;
	(void)next;

	if (semaphore_test_hook_active || semaphore_test_hook_runs != 0u) return;

	semaphore_test_hook_active = true;
	semaphore_test_hook_runs++;
	cr_assert_eq(sched_current_thread(), semaphore_test_signaler, "signaler should run while the waiter is blocked");
	cr_assert(semaphore_release(semaphore_test_semaphore), "semaphore_release should publish one permit");
	sched_yield();
	semaphore_test_hook_active = false;
}

static void semaphore_test_timeout_context_switch_hook(struct thread_context*       current,
                                                       const struct thread_context* next) {
	(void)current;
	(void)next;

	if (semaphore_test_hook_active || semaphore_test_hook_runs != 0u) return;

	semaphore_test_hook_active = true;
	semaphore_test_hook_runs++;
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_tick();
	(void)sched_handle_interrupt_exit();
	semaphore_test_hook_active = false;
}

Test(semaphore, init_try_acquire_release_and_count_track_permits) {
	struct semaphore semaphore;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	semaphore_init(&semaphore, 2u);
	cr_assert_eq(semaphore_count(&semaphore), 2u, "fresh semaphore should expose its initial permit count");
	cr_assert_eq(semaphore_waiter_count(&semaphore), 0u, "fresh semaphore should have no waiters");
	cr_assert(semaphore_try_acquire(&semaphore), "first try_acquire should consume one permit");
	cr_assert_eq(semaphore_count(&semaphore), 1u, "first acquire should decrement the permit count");
	cr_assert(semaphore_try_acquire(&semaphore), "second try_acquire should consume the last permit");
	cr_assert_eq(semaphore_count(&semaphore), 0u, "second acquire should empty the semaphore");
	cr_assert(!semaphore_try_acquire(&semaphore), "try_acquire should fail once no permits remain");
	cr_assert(semaphore_release(&semaphore), "release should restore one permit");
	cr_assert_eq(semaphore_count(&semaphore), 1u, "release should increment the permit count");

	reset_test_state();
}

Test(semaphore, release_rejects_overflow) {
	struct semaphore semaphore;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	semaphore_init(&semaphore, (size_t)-1);
	cr_assert(!semaphore_release(&semaphore), "release should reject permit counter overflow");
	cr_assert_eq(semaphore_count(&semaphore), (size_t)-1, "failed release must leave the count unchanged");

	reset_test_state();
}

Test(semaphore, acquire_blocks_and_release_wakes_waiter) {
	const struct thread_create_params waiter_params = {
		.name              = "semaphore_waiter",
		.entry             = semaphore_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x420000u,
		.kernel_stack_top  = 0x424000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params signaler_params = {
		.name              = "semaphore_signaler",
		.entry             = semaphore_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x430000u,
		.kernel_stack_top  = 0x434000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct semaphore semaphore;
	struct thread    waiter;
	struct thread    signaler;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	semaphore_init(&semaphore, 0u);
	cr_assert(thread_init(&waiter, &waiter_params), "waiter thread_init failed");
	cr_assert(thread_init(&signaler, &signaler_params), "signaler thread_init failed");
	cr_assert(sched_make_runnable(&signaler), "signaler should become runnable");

	semaphore_test_semaphore = &semaphore;
	semaphore_test_signaler  = &signaler;
	hal_cpu_mock_set_context_switch_hook(semaphore_test_signal_context_switch_hook);

	sched_set_current(cpu_current(), &waiter);
	semaphore_acquire(&semaphore);

	cr_assert_eq(semaphore_test_hook_runs, 1u, "signal hook should run exactly once");
	cr_assert_eq(sched_current_thread(), &waiter, "waiter should resume after release");
	cr_assert_eq(waiter.state, THREAD_STATE_RUNNING, "waiter should return to running after acquire completes");
	cr_assert_eq(semaphore_count(&semaphore), 0u, "woken waiter should consume the published permit");
	cr_assert_eq(semaphore_waiter_count(&semaphore), 0u, "wait queue should be empty after wake");

	reset_test_state();
}

Test(semaphore, timed_acquire_zero_timeout_is_non_blocking) {
	struct semaphore semaphore;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	semaphore_init(&semaphore, 1u);
	cr_assert(semaphore_timed_acquire(&semaphore, 0u), "zero-timeout timed acquire should consume an available permit");
	cr_assert_eq(semaphore_count(&semaphore), 0u, "timed acquire should decrement the permit count");
	cr_assert(!semaphore_timed_acquire(&semaphore, 0u), "zero-timeout timed acquire should fail when empty");
	cr_assert_eq(semaphore_waiter_count(&semaphore), 0u, "zero-timeout acquire should not enqueue a waiter");

	hal_clock_stop();
	reset_test_state();
}

Test(semaphore, timed_acquire_times_out_while_preemption_keeps_other_workers_running) {
	const struct thread_create_params waiter_params = {
		.name              = "semaphore_waiter",
		.entry             = semaphore_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x440000u,
		.kernel_stack_top  = 0x444000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner1_params = {
		.name              = "semaphore_runner1",
		.entry             = semaphore_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x450000u,
		.kernel_stack_top  = 0x454000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner2_params = {
		.name              = "semaphore_runner2",
		.entry             = semaphore_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x460000u,
		.kernel_stack_top  = 0x464000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct semaphore   semaphore;
	struct thread      waiter;
	struct thread      runner1;
	struct thread      runner2;
	struct sched_stats stats;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	semaphore_init(&semaphore, 0u);
	cr_assert(thread_init(&waiter, &waiter_params), "waiter thread_init failed");
	cr_assert(thread_init(&runner1, &runner1_params), "runner1 thread_init failed");
	cr_assert(thread_init(&runner2, &runner2_params), "runner2 thread_init failed");
	semaphore_test_set_one_tick_timeslice(&runner1);
	semaphore_test_set_one_tick_timeslice(&runner2);
	cr_assert(sched_make_runnable(&runner1), "runner1 should become runnable");
	cr_assert(sched_make_runnable(&runner2), "runner2 should become runnable");

	hal_cpu_mock_set_context_switch_hook(semaphore_test_timeout_context_switch_hook);
	sched_set_current(cpu_current(), &waiter);
	cr_assert(!semaphore_timed_acquire(&semaphore, 2u),
	          "timed acquire should time out while other workers keep running");
	cr_assert_eq(semaphore_test_hook_runs, 1u, "timeout simulation should run once");
	cr_assert_eq(semaphore_count(&semaphore), 0u, "timeout must not publish a permit");
	cr_assert_eq(semaphore_waiter_count(&semaphore), 0u, "timed out waiter should be removed from the queue");

	sched_get_stats(&stats);
	cr_assert_gt(stats.timeslice_preempt_count, 0u, "timer-driven preemption should occur during the timeout window");

	hal_clock_stop();
	reset_test_state();
}
