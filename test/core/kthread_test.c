#include <core/cpu.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <setjmp.h>

#include "../mocks/hal/cpu_mock.h"

static bool            kthread_test_cancel_hook_armed;
static jmp_buf         kthread_test_cancel_jmp;
static bool            kthread_test_sleep_cancel_hook_armed;
static struct kthread* kthread_test_sleep_cancel_target;

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
	hal_cpu_mock_set_thread_context_init_result(true);
	kthread_test_cancel_hook_armed       = false;
	kthread_test_sleep_cancel_hook_armed = false;
	kthread_test_sleep_cancel_target     = NULL;
	hal_cpu_local_bind(NULL);
}

static void kthread_test_entry(void* arg) {
	(void)arg;
}

static void kthread_test_cancel_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;

	if (!kthread_test_cancel_hook_armed) return;

	kthread_test_cancel_hook_armed = false;
	longjmp(kthread_test_cancel_jmp, 1);
}

static void kthread_test_sleep_cancel_context_switch_hook(struct thread_context*       current,
                                                          const struct thread_context* next) {
	(void)current;
	(void)next;

	if (!kthread_test_sleep_cancel_hook_armed || kthread_test_sleep_cancel_target == NULL) return;

	kthread_test_sleep_cancel_hook_armed = false;
	cr_assert(kthread_cancel(kthread_test_sleep_cancel_target),
	          "kthread_cancel should succeed while the sleeper is blocked");
}

Test(kthread, current_start_and_yield_delegate_to_scheduler) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};
	struct thread* idle;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	idle = sched_idle_thread(cpu_current());

	cr_assert_eq(kthread_current(), idle, "kthread_current should match scheduler current thread");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	cr_assert(sched_make_runnable(&worker.thread), "sched_make_runnable should queue the worker thread");

	kthread_yield();
	cr_assert_eq(kthread_current(), &worker.thread, "kthread_yield should dispatch the runnable worker thread");

	reset_test_state();
}

Test(kthread, init_reports_unsupported_context_setup) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x308000u,
		.kernel_stack_top  = 0x30c000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};

	hal_cpu_mock_set_thread_context_init_result(false);
	cr_assert_eq(thread_init_ex(&worker.thread, &params),
	             THREAD_INIT_CONTEXT_UNSUPPORTED,
	             "thread_init_ex should surface HAL bootstrap rejection");
	cr_assert(!thread_init(&worker.thread, &params), "thread_init should fail when bootstrap setup is unsupported");

	reset_test_state();
}

Test(kthread, join_terminated_thread_returns_exit_code_and_detaches) {
	const struct thread_create_params params = {
		.name              = "target",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread     target    = {.stack_id = VMM_ID_INVALID};
	thread_exit_code_t exit_code = 0u;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&target.thread, &params), "thread_init failed");

	thread_mark_exiting(&target.thread, 123u);
	thread_mark_zombie(&target.thread);
	cr_assert(kthread_join(&target, &exit_code), "kthread_join should succeed for terminated joinable thread");
	cr_assert_eq(exit_code, 123u, "kthread_join should publish target exit code");

	reset_test_state();
}

Test(kthread, join_detach_and_cancel_validate_inputs) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x320000u,
		.kernel_stack_top  = 0x324000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};
	struct thread  idle;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	thread_init_idle(&idle, cpu_current(), "idle/test");

	cr_assert(!kthread_join(NULL, NULL), "kthread_join should reject NULL targets");
	sched_set_current(cpu_current(), &worker.thread);
	cr_assert(!kthread_join(&worker, NULL), "kthread_join should reject self-join");
	sched_set_current(cpu_current(), &idle);
	cr_assert(thread_detach(&worker.thread), "thread_detach should succeed on live joinable thread");
	cr_assert(!kthread_join(&worker, NULL), "kthread_join should reject detached targets");
	cr_assert(kthread_cancel(&worker), "kthread_cancel should set cancellation pending on live thread");
	cr_assert(thread_cancel_requested(&worker.thread), "cancel flag should be visible after kthread_cancel");

	reset_test_state();
}

Test(kthread, canceled_running_thread_exits_with_cancel_code_at_cancellation_point) {
	const struct thread_create_params params = {
		.name              = "worker",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x330000u,
		.kernel_stack_top  = 0x334000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread worker = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&worker.thread, &params), "thread_init failed");
	sched_set_current(cpu_current(), &worker.thread);
	cr_assert(kthread_cancel(&worker), "kthread_cancel should mark the current worker for cancellation");

	hal_cpu_mock_set_context_switch_hook(kthread_test_cancel_context_switch_hook);
	kthread_test_cancel_hook_armed = true;
	if (setjmp(kthread_test_cancel_jmp) == 0) {
		kthread_yield();
		cr_assert_fail("kthread_yield should not return once cancellation exits the running thread");
	}

	cr_assert_eq(worker.thread.state, THREAD_STATE_ZOMBIE, "canceled worker should publish a zombie state");
	cr_assert_eq(worker.thread.exit_code,
	             THREAD_EXIT_CODE_CANCELLED,
	             "canceled worker should publish the dedicated cancellation exit code");
	cr_assert_eq(sched_current_thread(),
	             sched_idle_thread(cpu_current()),
	             "scheduler should switch away from the canceled worker");

	reset_test_state();
}

Test(kthread, cancel_wakes_sleeping_thread_so_it_can_reach_a_cancellation_point) {
	const struct thread_create_params params = {
		.name              = "sleeper",
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x340000u,
		.kernel_stack_top  = 0x344000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct kthread sleeper = {.stack_id = VMM_ID_INVALID};

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(thread_init(&sleeper.thread, &params), "thread_init failed");

	sched_set_current(cpu_current(), &sleeper.thread);
	kthread_test_sleep_cancel_target     = &sleeper;
	kthread_test_sleep_cancel_hook_armed = true;
	hal_cpu_mock_set_context_switch_hook(kthread_test_sleep_cancel_context_switch_hook);
	cr_assert(sched_sleep_until_tick(sched_tick_count() + 8u), "sched_sleep_until_tick failed");

	cr_assert(thread_cancel_requested(&sleeper.thread), "cancel flag should stay visible after wakeup");
	cr_assert_eq(sleeper.thread.state, THREAD_STATE_READY, "cancel should move the sleeping thread back to READY");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "canceled sleeper should be queued so it can run and exit");

	reset_test_state();
}
