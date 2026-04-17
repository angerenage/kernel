#include <core/condvar.h>
#include <core/cpu.h>
#include <core/mutex.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

static bool            condvar_test_hook_active;
static size_t          condvar_test_hook_runs;
static struct condvar* condvar_test_condvar;
static struct mutex*   condvar_test_mutex;
static struct thread*  condvar_test_signaler;

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
	condvar_test_hook_active = false;
	condvar_test_hook_runs   = 0u;
	condvar_test_condvar     = NULL;
	condvar_test_mutex       = NULL;
	condvar_test_signaler    = NULL;
	hal_cpu_local_bind(NULL);
}

static void condvar_test_entry(void* arg) {
	(void)arg;
}

static void condvar_test_signal_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;

	if (condvar_test_hook_active || condvar_test_hook_runs != 0u) return;

	condvar_test_hook_active = true;
	condvar_test_hook_runs++;
	cr_assert_eq(sched_current_thread(), condvar_test_signaler, "signaler should run while the waiter is blocked");
	cr_assert(mutex_try_lock(condvar_test_mutex), "signaler should acquire the mutex released by condvar_wait");
	cr_assert(condvar_signal(condvar_test_condvar), "condvar_signal should wake the blocked waiter");
	cr_assert(mutex_unlock(condvar_test_mutex), "signaler should release the mutex after signaling");
	sched_yield();
	condvar_test_hook_active = false;
}

static void condvar_test_timeout_context_switch_hook(struct thread_context*       current,
                                                     const struct thread_context* next) {
	(void)current;
	(void)next;

	if (condvar_test_hook_active || condvar_test_hook_runs != 0u) return;

	condvar_test_hook_active = true;
	condvar_test_hook_runs++;
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_yield();
	condvar_test_hook_active = false;
}

Test(condvar, init_signal_and_broadcast_handle_empty_wait_queue) {
	struct condvar condvar;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	condvar_init(&condvar);
	cr_assert_eq(thread_wait_queue_depth(&condvar.waiters), 0u, "fresh condvar should start with no waiters");
	cr_assert(!condvar_signal(&condvar), "signal should report no wakeup on an empty condvar");
	cr_assert_eq(condvar_broadcast(&condvar), 0u, "broadcast should report zero wakes on an empty condvar");

	reset_test_state();
}

Test(condvar, wait_releases_mutex_and_reacquires_it_after_signal) {
	const struct thread_create_params waiter_params = {
		.name              = "condvar_waiter",
		.entry             = condvar_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3e0000u,
		.kernel_stack_top  = 0x3e4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params signaler_params = {
		.name              = "condvar_signaler",
		.entry             = condvar_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x3f0000u,
		.kernel_stack_top  = 0x3f4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct condvar condvar;
	struct mutex   mutex;
	struct thread  waiter;
	struct thread  signaler;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	condvar_init(&condvar);
	mutex_init(&mutex);
	cr_assert(thread_init(&waiter, &waiter_params), "thread_init failed for waiter");
	cr_assert(thread_init(&signaler, &signaler_params), "thread_init failed for signaler");
	cr_assert(sched_make_runnable(&signaler), "failed to make signaler runnable");

	sched_set_current(cpu_current(), &waiter);
	cr_assert(mutex_try_lock(&mutex), "waiter should acquire the mutex before waiting");

	condvar_test_condvar  = &condvar;
	condvar_test_mutex    = &mutex;
	condvar_test_signaler = &signaler;
	hal_cpu_mock_set_context_switch_hook(condvar_test_signal_context_switch_hook);

	condvar_wait(&condvar, &mutex);

	cr_assert_eq(condvar_test_hook_runs, 1u, "signal hook should run exactly once");
	cr_assert_eq(sched_current_thread(), &waiter, "waiter should resume after the signal");
	cr_assert(mutex_is_owned_by_current(&mutex), "waiter should reacquire the mutex before returning");
	cr_assert_eq(thread_wait_queue_depth(&condvar.waiters), 0u, "condvar wait queue should be empty after wake");
	cr_assert(mutex_unlock(&mutex), "waiter should be able to unlock after the wait");

	reset_test_state();
}

Test(condvar, timed_wait_times_out_and_reacquires_mutex) {
	const struct thread_create_params waiter_params = {
		.name              = "timed_waiter",
		.entry             = condvar_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x400000u,
		.kernel_stack_top  = 0x404000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner_params = {
		.name              = "timeout_runner",
		.entry             = condvar_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x410000u,
		.kernel_stack_top  = 0x414000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct condvar condvar;
	struct mutex   mutex;
	struct thread  waiter;
	struct thread  runner;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	condvar_init(&condvar);
	mutex_init(&mutex);
	cr_assert(thread_init(&waiter, &waiter_params), "thread_init failed for waiter");
	cr_assert(thread_init(&runner, &runner_params), "thread_init failed for runner");
	cr_assert(sched_make_runnable(&runner), "failed to make timeout runner runnable");

	sched_set_current(cpu_current(), &waiter);
	cr_assert(mutex_try_lock(&mutex), "waiter should acquire the mutex before timed wait");

	hal_cpu_mock_set_context_switch_hook(condvar_test_timeout_context_switch_hook);
	cr_assert(!condvar_timed_wait(&condvar, &mutex, 2u), "timed wait should time out when no signal arrives");
	cr_assert_eq(condvar_test_hook_runs, 1u, "timeout hook should run exactly once");
	cr_assert_eq(sched_current_thread(), &waiter, "waiter should resume after timing out");
	cr_assert(mutex_is_owned_by_current(&mutex), "timed wait should reacquire the mutex before returning");
	cr_assert_eq(thread_wait_queue_depth(&condvar.waiters), 0u, "timed wait should remove the waiter on timeout");
	cr_assert(mutex_unlock(&mutex), "waiter should still be able to unlock after timeout");

	hal_clock_stop();
	reset_test_state();
}
