#include <core/cpu.h>
#include <core/kthread.h>
#include <core/mutex.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

static void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_local_bind(NULL);
}

static void mutex_test_entry(void* arg) {
	(void)arg;
}

Test(mutex, init_try_lock_and_unlock_track_owner_and_state) {
	struct mutex mutex;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	mutex_init(&mutex);
	cr_assert(!mutex_is_locked(&mutex), "fresh mutex should start unlocked");
	cr_assert_null(mutex_owner(&mutex), "fresh mutex should have no owner");
	cr_assert_eq(mutex_waiter_count(&mutex), 0u, "fresh mutex should have no waiters");
	cr_assert(mutex_try_lock(&mutex), "mutex_try_lock should acquire an unlocked mutex");
	cr_assert(mutex_is_locked(&mutex), "mutex should report locked after try_lock");
	cr_assert(mutex_is_owned_by_current(&mutex), "current thread should own the mutex after try_lock");
	cr_assert_eq(mutex_owner(&mutex), sched_current_thread(), "owner should match the current thread");
	cr_assert(!mutex_try_lock(&mutex), "mutex_try_lock should reject recursive acquisition");
	cr_assert(mutex_unlock(&mutex), "mutex_unlock should release the owned mutex");
	cr_assert(!mutex_is_locked(&mutex), "mutex should report unlocked after unlock");
	cr_assert_null(mutex_owner(&mutex), "mutex owner should clear after unlock");

	reset_test_state();
}

Test(mutex, unlock_rejects_non_owner) {
	const struct thread_create_params params = {
		.name              = "mutex_owner",
		.entry             = mutex_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x350000u,
		.kernel_stack_top  = 0x354000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct mutex  mutex;
	struct thread owner;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	mutex_init(&mutex);
	cr_assert(kthread_create(&owner, &params), "kthread_create failed");
	sched_set_current(cpu_current(), &owner);
	cr_assert(mutex_try_lock(&mutex), "owner thread should acquire the mutex");
	cr_assert_eq(mutex_owner(&mutex), &owner, "owner thread should hold the mutex");

	sched_set_current(cpu_current(), sched_idle_thread(cpu_current()));
	cr_assert(!mutex_is_owned_by_current(&mutex), "idle thread should not own the worker-held mutex");
	cr_assert(!mutex_unlock(&mutex), "non-owner unlock should fail");
	cr_assert(mutex_is_locked(&mutex), "mutex should remain locked after a rejected unlock");
	cr_assert_eq(mutex_owner(&mutex), &owner, "rejected unlock must not change ownership");

	reset_test_state();
}

Test(mutex, contended_lock_blocks_and_wakes_waiter) {
	const struct thread_create_params owner_params = {
		.name              = "mutex_owner",
		.entry             = mutex_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x360000u,
		.kernel_stack_top  = 0x364000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params waiter_params = {
		.name              = "mutex_waiter",
		.entry             = mutex_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x370000u,
		.kernel_stack_top  = 0x374000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct mutex  mutex;
	struct thread owner;
	struct thread waiter;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	mutex_init(&mutex);
	cr_assert(kthread_create(&owner, &owner_params), "owner kthread_create failed");
	cr_assert(kthread_create(&waiter, &waiter_params), "waiter kthread_create failed");

	sched_set_current(cpu_current(), &owner);
	cr_assert(mutex_try_lock(&mutex), "owner should acquire the mutex");
	cr_assert_eq(mutex_owner(&mutex), &owner, "owner should hold the mutex");

	sched_set_current(cpu_current(), &waiter);
	cr_assert(!mutex_try_lock(&mutex), "waiter try_lock should fail while owner holds the mutex");
	sched_block_current(&mutex.waiters, THREAD_BLOCK_MUTEX);
	cr_assert_eq(waiter.state, THREAD_STATE_BLOCKED, "waiter should become blocked on the mutex");
	cr_assert_eq(waiter.block_reason, THREAD_BLOCK_MUTEX, "waiter block reason should record mutex contention");
	cr_assert_eq(mutex_waiter_count(&mutex), 1u, "mutex should report one blocked waiter");
	cr_assert_eq(sched_current_thread(),
	             sched_idle_thread(cpu_current()),
	             "blocking the sole runnable waiter should leave the CPU on idle");

	sched_set_current(cpu_current(), &owner);
	cr_assert(mutex_unlock(&mutex), "owner unlock should wake one waiter");
	cr_assert_eq(waiter.state, THREAD_STATE_READY, "woken waiter should return to READY");
	cr_assert_eq(mutex_waiter_count(&mutex), 0u, "mutex wait queue should be empty after wake");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "woken waiter should be queued to run");

	kthread_yield();
	cr_assert_eq(sched_current_thread(), &waiter, "yield should dispatch the woken waiter");
	mutex_lock(&mutex);
	cr_assert(mutex_is_owned_by_current(&mutex), "woken waiter should acquire the unlocked mutex");
	cr_assert_eq(mutex_owner(&mutex), &waiter, "waiter should become the new owner after lock");
	cr_assert(mutex_unlock(&mutex), "waiter should be able to release the mutex");
	cr_assert(!mutex_is_locked(&mutex), "mutex should end unlocked");

	reset_test_state();
}

Test(mutex, timed_lock_zero_timeout_acquires_without_sleeping) {
	struct mutex mutex;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");

	mutex_init(&mutex);
	cr_assert(mutex_timed_lock(&mutex, 0u), "zero-timeout timed lock should acquire an unlocked mutex");
	cr_assert(mutex_is_owned_by_current(&mutex), "current thread should own the mutex after timed lock");
	cr_assert(mutex_unlock(&mutex), "mutex_unlock should release the timed lock");

	reset_test_state();
}

Test(mutex, timed_lock_zero_timeout_times_out_when_contended) {
	const struct thread_create_params owner_params = {
		.name              = "mutex_owner",
		.entry             = mutex_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x380000u,
		.kernel_stack_top  = 0x384000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params waiter_params = {
		.name              = "mutex_waiter",
		.entry             = mutex_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x390000u,
		.kernel_stack_top  = 0x394000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct mutex  mutex;
	struct thread owner;
	struct thread waiter;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");

	mutex_init(&mutex);
	cr_assert(kthread_create(&owner, &owner_params), "owner kthread_create failed");
	cr_assert(kthread_create(&waiter, &waiter_params), "waiter kthread_create failed");

	sched_set_current(cpu_current(), &owner);
	cr_assert(mutex_try_lock(&mutex), "owner should acquire the mutex");

	sched_set_current(cpu_current(), &waiter);
	cr_assert(!mutex_timed_lock(&mutex, 0u), "zero-timeout timed lock should fail while another thread owns the mutex");
	cr_assert_eq(mutex_owner(&mutex), &owner, "failed timed lock must not change ownership");
	cr_assert_eq(mutex_waiter_count(&mutex), 0u, "zero-timeout timed lock should not enqueue a waiter");

	sched_set_current(cpu_current(), &owner);
	cr_assert(mutex_unlock(&mutex), "owner should still be able to unlock");
	hal_clock_stop();
	reset_test_state();
}
