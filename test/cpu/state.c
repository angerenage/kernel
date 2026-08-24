#include <core/thread.h>

#include "test_support.h"

Test(cpu, current_thread_snapshot_uses_the_explicit_publication_api) {
	struct thread published = {0};
	struct cpu*   ap;

	cr_assert(cpu_topology_init(cpu_test_valid_topology, 2u, 0u), "topology init failed");
	ap = cpu_by_index(1u);
	cr_assert_not_null(ap, "AP lookup failed");
	cpu_bind_current(ap);
	cr_assert_null(cpu_current_thread_load(ap), "new CPU must not publish a current thread");

	cpu_current_thread_store(ap, &published);
	cr_assert_eq(cpu_current_thread_load(ap), &published, "published current-thread snapshot mismatch");

	cpu_current_thread_store(ap, NULL);
	cr_assert_null(cpu_current_thread_load(ap), "cleared current-thread snapshot remained visible");
	cpu_test_reset();
}

Test(cpu_irq, state_transitions_track_online_count_without_double_counting) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 10u,
         .arch_id         = 0x20u,
         .role            = CPU_ROLE_BSP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
		{
         .index           = 1u,
         .processor_id    = 11u,
         .arch_id         = 0x21u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x210000u,
         .boot_stack_top  = 0x214000u,
		 },
	};
	struct cpu* bsp;
	struct cpu* ap;

	cr_assert(cpu_topology_init(init_info, 2u, 0u), "cpu_topology_init failed");

	bsp = cpu_bsp();
	ap  = cpu_by_index(1u);
	cr_assert_not_null(bsp, "cpu_bsp returned NULL");
	cr_assert_not_null(ap, "cpu_by_index returned NULL for the AP");
	cr_assert_eq(cpu_online_count(), 0u, "online count should start at zero");

	cr_assert(cpu_set_state(bsp, CPU_STATE_ONLINE), "cpu_set_state failed for BSP");
	cr_assert_eq(cpu_state_get(bsp), CPU_STATE_ONLINE, "BSP state did not become ONLINE");
	cr_assert_eq(cpu_online_count(), 1u, "online count did not increase for the BSP");

	cr_assert(cpu_set_state(bsp, CPU_STATE_ONLINE), "repeating ONLINE transition should still succeed");
	cr_assert_eq(cpu_online_count(), 1u, "repeating ONLINE transition double-counted the BSP");

	cr_assert(cpu_set_state(ap, CPU_STATE_ONLINE), "cpu_set_state failed for AP");
	cr_assert_eq(cpu_online_count(), 2u, "online count did not increase for the AP");

	cr_assert(cpu_set_state(bsp, CPU_STATE_PARKED), "cpu_set_state failed for BSP park");
	cr_assert_eq(cpu_online_count(), 1u, "online count did not decrease when BSP left ONLINE");

	cr_assert(cpu_set_state(bsp, CPU_STATE_PARKED), "repeating non-ONLINE transition should succeed");
	cr_assert_eq(cpu_online_count(), 1u, "repeating non-ONLINE transition changed online count");

	cr_assert(cpu_set_state(ap, CPU_STATE_HALTED), "cpu_set_state failed for AP halt");
	cr_assert_eq(cpu_online_count(), 0u, "online count did not drop back to zero");
	cr_assert(!cpu_set_state(NULL, CPU_STATE_ONLINE), "cpu_set_state accepted NULL");
	cr_assert_eq(cpu_state_get(NULL), CPU_STATE_HALTED, "cpu_state_get(NULL) should report HALTED");
}

Test(cpu, state_changes_reject_descriptors_outside_the_installed_topology) {
	struct cpu foreign = {
		.index = 0u,
		.state = CPU_STATE_PRESENT,
	};

	cr_assert(cpu_topology_init(cpu_test_valid_topology, 2u, 0u), "topology init failed");
	cr_assert_eq(cpu_online_count(), 0u, "fresh topology must start fully offline");

	cr_assert_not(cpu_set_state(&foreign, CPU_STATE_ONLINE), "foreign CPU descriptors must not mutate topology state");
	cr_assert_eq(cpu_online_count(), 0u, "foreign descriptor must not change topology online count");
	cr_assert_eq(
		cpu_state_get(&foreign), CPU_STATE_PRESENT, "failed foreign transition must not mutate the descriptor");

	cr_assert(cpu_set_state(cpu_by_index(0u), CPU_STATE_ONLINE), "installed CPU must accept ONLINE transition");
	cr_assert_eq(cpu_online_count(), 1u, "installed ONLINE transition must increment count once");

	cr_assert_not(cpu_set_state(&foreign, CPU_STATE_HALTED),
	              "foreign CPU descriptor must remain rejected after topology changes");
	cr_assert_eq(cpu_online_count(), 1u, "foreign transition must not corrupt an existing online count");

	cpu_test_reset();
}
