#include "test_support.h"

void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_cpu_local_bind(NULL);
}

void thread_test_entry(void* arg) {
	(void)arg;
}
void thread_regression_reset(void) {
	reset_test_state();
}

void thread_regression_init(struct thread* thread, const char* name, uintptr_t stack_base, int32_t priority) {
	const struct thread_create_params params = {.name              = name,
	                                            .entry             = thread_test_entry,
	                                            .arg               = NULL,
	                                            .kernel_stack_base = stack_base,
	                                            .kernel_stack_top  = stack_base + 0x4000u,
	                                            .preferred_cpu     = NULL,
	                                            .base_priority     = priority,
	                                            .detached          = false};
	cr_assert(thread_init(thread, &params), "thread_init failed for %s", name);
}
