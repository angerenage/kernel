#include "test_support.h"

bool            kthread_test_cancel_hook_armed;
jmp_buf         kthread_test_cancel_jmp;
bool            kthread_test_sleep_cancel_hook_armed;
struct kthread* kthread_test_sleep_cancel_target;
bool            kthread_test_park_hook_armed;
struct kthread* kthread_test_park_target;
size_t          kthread_test_timeout_hook_runs;
size_t          kthread_test_park_hook_runs;

static bool kthread_test_timeout_hook_active;

void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	kthread_test_cancel_hook_armed       = false;
	kthread_test_sleep_cancel_hook_armed = false;
	kthread_test_sleep_cancel_target     = NULL;
	kthread_test_park_hook_armed         = false;
	kthread_test_park_target             = NULL;
	kthread_test_timeout_hook_active     = false;
	kthread_test_timeout_hook_runs       = 0u;
	kthread_test_park_hook_runs          = 0u;
	hal_cpu_local_bind(NULL);
}

void kthread_test_init_scheduler(void) {
	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
}

void kthread_test_entry(void* arg) {
	(void)arg;
}

void kthread_test_init_target(struct kthread* target, const char* name, uintptr_t stack_base, bool make_current) {
	const struct thread_create_params params = {
		.name              = name,
		.entry             = kthread_test_entry,
		.arg               = NULL,
		.kernel_stack_base = stack_base,
		.kernel_stack_top  = stack_base + 0x4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};

	*target = (struct kthread){.stack_id = VMM_ID_INVALID};
	cr_assert(thread_init(&target->thread, &params));
	if (make_current) sched_set_current(cpu_current(), &target->thread);
}

void kthread_test_cancel_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;
	if (!kthread_test_cancel_hook_armed) return;
	kthread_test_cancel_hook_armed = false;
	longjmp(kthread_test_cancel_jmp, 1);
}

void kthread_test_sleep_cancel_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;
	if (!kthread_test_sleep_cancel_hook_armed || kthread_test_sleep_cancel_target == NULL) return;
	kthread_test_sleep_cancel_hook_armed = false;
	cr_assert(kthread_cancel(kthread_test_sleep_cancel_target));
}

void kthread_test_park_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;
	if (!kthread_test_park_hook_armed || kthread_test_park_target == NULL) return;
	kthread_test_park_hook_armed = false;
	kthread_test_park_hook_runs++;
	cr_assert(kthread_unpark(kthread_test_park_target));
	sched_yield();
}

void kthread_test_timeout_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;
	if (kthread_test_timeout_hook_active || kthread_test_timeout_hook_runs != 0u) return;
	kthread_test_timeout_hook_active = true;
	kthread_test_timeout_hook_runs++;
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_tick();
	(void)sched_handle_interrupt_exit();
	sched_yield();
	kthread_test_timeout_hook_active = false;
}
