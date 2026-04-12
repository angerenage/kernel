#include <core/cpu.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
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

static void kthread_test_entry(void* arg) {
	(void)arg;
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
	struct thread  worker;
	struct thread* idle;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	idle = sched_idle_thread(cpu_current());

	cr_assert_eq(kthread_current(), idle, "kthread_current should match scheduler current thread");
	cr_assert(kthread_create(&worker, &params), "kthread_create failed");
	cr_assert(kthread_start(&worker), "kthread_start should make thread runnable");

	kthread_yield();
	cr_assert_eq(kthread_current(), &worker, "kthread_yield should dispatch the runnable worker thread");

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
	struct thread      target;
	thread_exit_code_t exit_code = 0u;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(kthread_create(&target, &params), "kthread_create failed");

	thread_mark_exiting(&target, 123u);
	thread_mark_zombie(&target);
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
	struct thread worker;
	struct thread idle;

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	cr_assert(kthread_create(&worker, &params), "kthread_create failed");
	thread_init_idle(&idle, cpu_current(), "idle/test");

	cr_assert(!kthread_join(NULL, NULL), "kthread_join should reject NULL targets");
	cr_assert(!kthread_join(kthread_current(), NULL), "kthread_join should reject self-join");
	cr_assert(!kthread_join(&idle, NULL), "kthread_join should reject idle thread targets");
	cr_assert(kthread_detach(&worker), "kthread_detach should succeed on live joinable thread");
	cr_assert(!kthread_join(&worker, NULL), "kthread_join should reject detached targets");
	cr_assert(kthread_cancel(&worker), "kthread_cancel should set cancellation pending on live thread");
	cr_assert(thread_cancel_requested(&worker), "cancel flag should be visible after kthread_cancel");

	reset_test_state();
}
