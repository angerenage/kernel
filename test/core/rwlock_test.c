#include <core/cpu.h>
#include <core/kthread.h>
#include <core/rwlock.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

static bool           rwlock_test_hook_active;
static size_t         rwlock_test_hook_runs;
static struct rwlock* rwlock_test_lock;
static struct thread* rwlock_test_reader;

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
	rwlock_test_hook_active = false;
	rwlock_test_hook_runs   = 0u;
	rwlock_test_lock        = NULL;
	rwlock_test_reader      = NULL;
	hal_cpu_local_bind(NULL);
}

static void rwlock_test_entry(void* arg) {
	(void)arg;
}

static void rwlock_test_set_one_tick_timeslice(struct thread* thread) {
	if (thread == NULL) return;

	thread->timeslice_ticks     = 1u;
	thread->timeslice_remaining = 1u;
}

static void rwlock_test_writer_wake_context_switch_hook(struct thread_context*       current,
                                                        const struct thread_context* next) {
	(void)current;
	(void)next;

	if (rwlock_test_hook_active || rwlock_test_hook_runs != 0u) return;

	rwlock_test_hook_active = true;
	rwlock_test_hook_runs++;
	cr_assert_eq(sched_current_thread(), rwlock_test_reader, "reader should run while the writer is blocked");
	cr_assert_eq(rwlock_reader_count(rwlock_test_lock), 1u, "reader count should stay published while writer waits");
	cr_assert_eq(rwlock_waiter_count(rwlock_test_lock), 1u, "writer should be the only blocked waiter");
	cr_assert(rwlock_read_unlock(rwlock_test_lock), "last reader should be able to wake the blocked writer");
	sched_yield();
	rwlock_test_hook_active = false;
}

static void rwlock_test_timeout_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;

	if (rwlock_test_hook_active || rwlock_test_hook_runs != 0u) return;

	rwlock_test_hook_active = true;
	rwlock_test_hook_runs++;
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_tick();
	(void)sched_handle_interrupt_exit();
	rwlock_test_hook_active = false;
}

Test(rwlock, init_try_paths_and_unlocks_track_state) {
	struct rwlock rwlock;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	rwlock_init(&rwlock);
	cr_assert_eq(rwlock_reader_count(&rwlock), 0u, "fresh rwlock should start without active readers");
	cr_assert_eq(rwlock_waiter_count(&rwlock), 0u, "fresh rwlock should start without waiters");
	cr_assert(rwlock_try_read_lock(&rwlock), "try_read_lock should acquire an unlocked rwlock");
	cr_assert(rwlock_try_read_lock(&rwlock), "multiple readers should share the rwlock");
	cr_assert_eq(rwlock_reader_count(&rwlock), 2u, "reader count should track shared holders");
	cr_assert(!rwlock_try_write_lock(&rwlock), "writer try lock should fail while readers hold the rwlock");
	cr_assert(rwlock_read_unlock(&rwlock), "first reader unlock should succeed");
	cr_assert(rwlock_read_unlock(&rwlock), "second reader unlock should succeed");
	cr_assert_eq(rwlock_reader_count(&rwlock), 0u, "reader count should drop back to zero");
	cr_assert(rwlock_try_write_lock(&rwlock), "write try lock should acquire once readers leave");
	cr_assert(!rwlock_try_read_lock(&rwlock), "new readers should be blocked while a writer owns the rwlock");
	cr_assert(rwlock_downgrade(&rwlock), "writer should be able to downgrade to a read lock");
	cr_assert_eq(rwlock_reader_count(&rwlock), 1u, "downgrade should publish one reader hold");
	cr_assert(!rwlock_try_write_lock(&rwlock), "downgraded read hold should block writers");
	cr_assert(rwlock_try_read_lock(&rwlock), "other readers should share a downgraded lock when no writer waits");
	cr_assert_eq(rwlock_reader_count(&rwlock), 2u, "downgraded lock should count every reader hold");
	cr_assert(rwlock_read_unlock(&rwlock), "extra reader unlock should succeed");
	cr_assert(rwlock_read_unlock(&rwlock), "downgraded reader unlock should succeed");
	cr_assert_eq(rwlock_reader_count(&rwlock), 0u, "rwlock should end without active readers");
	cr_assert_eq(rwlock_waiter_count(&rwlock), 0u, "rwlock should end without waiters");

	reset_test_state();
}

Test(rwlock, write_unlock_rejects_non_owner) {
	const struct thread_create_params owner_params = {
		.name              = "rwlock_owner",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x470000u,
		.kernel_stack_top  = 0x474000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct rwlock rwlock;
	struct thread owner;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	rwlock_init(&rwlock);
	cr_assert(thread_init(&owner, &owner_params), "thread_init failed");
	sched_set_current(cpu_current(), &owner);
	cr_assert(rwlock_try_write_lock(&rwlock), "owner thread should acquire the write lock");

	sched_set_current(cpu_current(), sched_idle_thread(cpu_current()));
	cr_assert(!rwlock_write_unlock(&rwlock), "non-owner write unlock should fail");
	cr_assert(!rwlock_downgrade(&rwlock), "non-owner downgrade should fail");

	sched_set_current(cpu_current(), &owner);
	cr_assert(rwlock_write_unlock(&rwlock), "owner should still be able to release the write lock");

	reset_test_state();
}

Test(rwlock, write_lock_blocks_until_last_reader_unlocks) {
	const struct thread_create_params reader_params = {
		.name              = "rwlock_reader",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x480000u,
		.kernel_stack_top  = 0x484000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params writer_params = {
		.name              = "rwlock_writer",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x490000u,
		.kernel_stack_top  = 0x494000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct rwlock rwlock;
	struct thread reader;
	struct thread writer;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	rwlock_init(&rwlock);
	cr_assert(thread_init(&reader, &reader_params), "reader thread_init failed");
	cr_assert(thread_init(&writer, &writer_params), "writer thread_init failed");

	sched_set_current(cpu_current(), &reader);
	cr_assert(rwlock_try_read_lock(&rwlock), "reader should acquire the shared lock");

	sched_set_current(cpu_current(), sched_idle_thread(cpu_current()));
	cr_assert(sched_make_runnable(&reader), "reader should be runnable again to release the lock");

	rwlock_test_lock   = &rwlock;
	rwlock_test_reader = &reader;
	hal_cpu_mock_set_context_switch_hook(rwlock_test_writer_wake_context_switch_hook);

	sched_set_current(cpu_current(), &writer);
	rwlock_write_lock(&rwlock);

	cr_assert_eq(rwlock_test_hook_runs, 1u, "reader release hook should run exactly once");
	cr_assert_eq(sched_current_thread(), &writer, "writer should resume after the last reader unlocks");
	cr_assert_eq(rwlock_reader_count(&rwlock), 0u, "writer acquisition should wait for every reader to leave");
	cr_assert_eq(rwlock_waiter_count(&rwlock), 0u, "wait queues should be empty once the writer acquires");
	cr_assert(rwlock_write_unlock(&rwlock), "writer should be able to release the write lock");

	reset_test_state();
}

Test(rwlock, timed_read_lock_times_out_while_preemption_keeps_other_workers_running) {
	const struct thread_create_params writer_params = {
		.name              = "rwlock_writer",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4a0000u,
		.kernel_stack_top  = 0x4a4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params reader_params = {
		.name              = "rwlock_reader",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4b0000u,
		.kernel_stack_top  = 0x4b4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner1_params = {
		.name              = "rwlock_runner1",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4c0000u,
		.kernel_stack_top  = 0x4c4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner2_params = {
		.name              = "rwlock_runner2",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4d0000u,
		.kernel_stack_top  = 0x4d4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct rwlock      rwlock;
	struct thread      writer;
	struct thread      reader;
	struct thread      runner1;
	struct thread      runner2;
	struct sched_stats stats;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	rwlock_init(&rwlock);
	cr_assert(thread_init(&writer, &writer_params), "writer thread_init failed");
	cr_assert(thread_init(&reader, &reader_params), "reader thread_init failed");
	cr_assert(thread_init(&runner1, &runner1_params), "runner1 thread_init failed");
	cr_assert(thread_init(&runner2, &runner2_params), "runner2 thread_init failed");
	rwlock_test_set_one_tick_timeslice(&runner1);
	rwlock_test_set_one_tick_timeslice(&runner2);

	sched_set_current(cpu_current(), &writer);
	cr_assert(rwlock_try_write_lock(&rwlock), "writer should acquire the write lock");
	cr_assert(sched_make_runnable(&runner1), "runner1 should become runnable");
	cr_assert(sched_make_runnable(&runner2), "runner2 should become runnable");

	hal_cpu_mock_set_context_switch_hook(rwlock_test_timeout_context_switch_hook);
	sched_set_current(cpu_current(), &reader);
	cr_assert(!rwlock_timed_read_lock(&rwlock, 2u), "timed read lock should time out while a writer holds the rwlock");
	cr_assert_eq(rwlock_test_hook_runs, 1u, "timeout hook should run exactly once");
	cr_assert_eq(rwlock_reader_count(&rwlock), 0u, "timed out reader must not publish a read hold");
	cr_assert_eq(rwlock_waiter_count(&rwlock), 0u, "timed out reader should be removed from the wait queue");

	sched_get_stats(&stats);
	cr_assert_gt(stats.timeslice_preempt_count, 0u, "timer-driven preemption should occur during the timeout window");

	sched_set_current(cpu_current(), &writer);
	cr_assert(rwlock_write_unlock(&rwlock), "writer should still be able to unlock after the timeout");
	hal_clock_stop();
	reset_test_state();
}

Test(rwlock, timed_write_lock_times_out_while_readers_hold_the_rwlock) {
	const struct thread_create_params reader_params = {
		.name              = "rwlock_reader",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4e0000u,
		.kernel_stack_top  = 0x4e4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params writer_params = {
		.name              = "rwlock_writer",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4f0000u,
		.kernel_stack_top  = 0x4f4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner1_params = {
		.name              = "rwlock_runner1",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x500000u,
		.kernel_stack_top  = 0x504000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params runner2_params = {
		.name              = "rwlock_runner2",
		.entry             = rwlock_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x510000u,
		.kernel_stack_top  = 0x514000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct rwlock      rwlock;
	struct thread      reader;
	struct thread      writer;
	struct thread      runner1;
	struct thread      runner2;
	struct sched_stats stats;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	rwlock_init(&rwlock);
	cr_assert(thread_init(&reader, &reader_params), "reader thread_init failed");
	cr_assert(thread_init(&writer, &writer_params), "writer thread_init failed");
	cr_assert(thread_init(&runner1, &runner1_params), "runner1 thread_init failed");
	cr_assert(thread_init(&runner2, &runner2_params), "runner2 thread_init failed");
	rwlock_test_set_one_tick_timeslice(&runner1);
	rwlock_test_set_one_tick_timeslice(&runner2);

	sched_set_current(cpu_current(), &reader);
	cr_assert(rwlock_try_read_lock(&rwlock), "reader should acquire the shared lock");
	cr_assert(sched_make_runnable(&runner1), "runner1 should become runnable");
	cr_assert(sched_make_runnable(&runner2), "runner2 should become runnable");

	hal_cpu_mock_set_context_switch_hook(rwlock_test_timeout_context_switch_hook);
	sched_set_current(cpu_current(), &writer);
	cr_assert(!rwlock_timed_write_lock(&rwlock, 2u),
	          "timed write lock should time out while a reader still holds the rwlock");
	cr_assert_eq(rwlock_test_hook_runs, 1u, "timeout hook should run exactly once");
	cr_assert_eq(rwlock_reader_count(&rwlock), 1u, "timed out writer must not disturb active readers");
	cr_assert_eq(rwlock_waiter_count(&rwlock), 0u, "timed out writer should be removed from the wait queue");

	sched_get_stats(&stats);
	cr_assert_gt(stats.timeslice_preempt_count, 0u, "timer-driven preemption should occur during the timeout window");

	sched_set_current(cpu_current(), &reader);
	cr_assert(rwlock_read_unlock(&rwlock), "reader should still be able to unlock after the timeout");
	hal_clock_stop();
	reset_test_state();
}
