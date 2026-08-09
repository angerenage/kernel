#include "test_support.h"

bool   sched_regression_reschedule_hook_armed;
size_t sched_regression_reap_count;

void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
}

void reset_test_state(void) {
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
	hal_paging_mock_reset_active();
}

void sched_test_thread_entry(void* arg) {
	(void)arg;
}

void sched_test_set_one_tick_timeslice(struct thread* thread) {
	if (thread == NULL) return;
	thread->timeslice_ticks     = 1u;
	thread->timeslice_remaining = 1u;
}

void init_started_dual_cpu_topology(struct cpu** out_bsp, struct cpu** out_ap) {
	const struct cpu_init_info info[] = {
		{.index           = 0u,
	     .processor_id    = 10u,
	     .arch_id         = 0x20u,
	     .role            = CPU_ROLE_BSP,
	     .boot_stack_base = 0x200000u,
	     .boot_stack_top  = 0x204000u},
		{.index           = 1u,
	     .processor_id    = 11u,
	     .arch_id         = 0x21u,
	     .role            = CPU_ROLE_AP,
	     .boot_stack_base = 0x210000u,
	     .boot_stack_top  = 0x214000u},
	};
	struct cpu* bsp;
	struct cpu* ap;
	cr_assert(cpu_topology_init(info, 2u, 0u));
	bsp = cpu_bsp();
	ap  = cpu_by_index(1u);
	cr_assert_not_null(bsp);
	cr_assert_not_null(ap);
	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(bsp));
	cpu_bind_current(ap);
	cpu_interrupts_set_ready(ap, false);
	cr_assert(sched_start_cpu(ap));
	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	if (out_bsp != NULL) *out_bsp = bsp;
	if (out_ap != NULL) *out_ap = ap;
}

void sched_regression_reset(void) {
	irq_enable_local();
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_reset_kicks();
	hal_cpu_local_bind(NULL);
	sched_regression_reschedule_hook_armed = false;
	sched_regression_reap_count            = 0u;
}

void sched_regression_init_single_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE));
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(cpu_current()));
}

void sched_regression_init_dual_cpu(struct cpu** out_bsp, struct cpu** out_ap) {
	const struct cpu_init_info info[] = {
		{.index           = 0u,
	     .processor_id    = 10u,
	     .arch_id         = 0x20u,
	     .role            = CPU_ROLE_BSP,
	     .boot_stack_base = 0x200000u,
	     .boot_stack_top  = 0x204000u},
		{.index           = 1u,
	     .processor_id    = 11u,
	     .arch_id         = 0x21u,
	     .role            = CPU_ROLE_AP,
	     .boot_stack_base = 0x210000u,
	     .boot_stack_top  = 0x214000u},
	};
	struct cpu* bsp;
	struct cpu* ap;
	cr_assert(cpu_topology_init(info, 2u, 0u));
	bsp = cpu_bsp();
	ap  = cpu_by_index(1u);
	cr_assert_not_null(bsp);
	cr_assert_not_null(ap);
	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	cr_assert(cpu_set_state(bsp, CPU_STATE_ONLINE));
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(bsp));
	cpu_bind_current(ap);
	cpu_interrupts_set_ready(ap, false);
	cr_assert(cpu_set_state(ap, CPU_STATE_ONLINE));
	cr_assert(sched_start_cpu(ap));
	cpu_bind_current(bsp);
	cpu_interrupts_set_ready(bsp, false);
	if (out_bsp != NULL) *out_bsp = bsp;
	if (out_ap != NULL) *out_ap = ap;
}

void sched_regression_init_thread(struct thread* thread, const char* name, uintptr_t stack_base, int32_t priority,
                                  struct cpu* preferred_cpu, struct address_space* address_space) {
	const struct thread_create_params params = {.name              = name,
	                                            .entry             = sched_test_thread_entry,
	                                            .arg               = NULL,
	                                            .kernel_stack_base = stack_base,
	                                            .kernel_stack_top  = stack_base + 0x4000u,
	                                            .address_space     = address_space,
	                                            .preferred_cpu     = preferred_cpu,
	                                            .base_priority     = priority,
	                                            .detached          = false};
	cr_assert(thread_init(thread, &params), "thread_init failed for %s", name);
}

void sched_regression_reschedule_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;
	if (!sched_regression_reschedule_hook_armed) return;
	sched_regression_reschedule_hook_armed = false;
	sched_request_reschedule(cpu_current());
}

void sched_regression_reap_callback(struct thread* thread, void* ctx) {
	(void)thread;
	(void)ctx;
	sched_regression_reap_count++;
}
