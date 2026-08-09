#include "test_support.h"

Test(cpu, topology_initializes_from_generic_descriptors) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 10u,
         .arch_id         = 0x20u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x210000u,
         .boot_stack_top  = 0x214000u,
		 },
		{
         .index           = 1u,
         .processor_id    = 11u,
         .arch_id         = 0x21u,
         .role            = CPU_ROLE_BSP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
		{
         .index           = 2u,
         .processor_id    = 12u,
         .arch_id         = 0x22u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x220000u,
         .boot_stack_top  = 0x224000u,
		 },
	};
	struct cpu_topology* topology;
	struct cpu*          bsp;
	struct cpu*          ap0;
	struct cpu*          ap2;

	cr_assert(cpu_topology_init(init_info, 3u, 1u), "cpu_topology_init failed");

	topology = cpu_topology_get();
	bsp      = cpu_bsp();
	ap0      = cpu_by_index(0u);
	ap2      = cpu_by_index(2u);

	cr_assert_not_null(topology, "cpu_topology_get returned NULL");
	cr_assert_eq(cpu_count(), 3u, "cpu_count did not match the generic topology input");
	cr_assert_eq(cpu_online_count(), 0u, "online CPUs should be zero before boot publication");
	cr_assert_eq(topology->bsp_index, 1u, "BSP index did not match the generic topology input");
	cr_assert_not_null(bsp, "cpu_bsp returned NULL");
	cr_assert_eq(bsp->index, 1u, "cpu_bsp selected the wrong CPU");
	cr_assert_eq(bsp->arch_id, 0x21u, "cpu_bsp arch id mismatch");
	cr_assert_eq(bsp->processor_id, 11u, "cpu_bsp processor id mismatch");
	cr_assert_eq(bsp->boot_stack_base, 0x200000u, "BSP boot stack base mismatch");
	cr_assert_eq(bsp->boot_stack_top, 0x204000u, "BSP boot stack top mismatch");
	cr_assert_not_null(ap0, "first AP lookup failed");
	cr_assert_not_null(ap2, "second AP lookup failed");
	cr_assert_eq(ap0->role, CPU_ROLE_AP, "CPU0 should be marked as an AP");
	cr_assert_eq(ap2->role, CPU_ROLE_AP, "CPU2 should be marked as an AP");
	cr_assert_eq(ap0->boot_stack_top - ap0->boot_stack_base, 0x4000u, "AP0 boot stack size mismatch");
	cr_assert_eq(ap2->boot_stack_top - ap2->boot_stack_base, 0x4000u, "AP2 boot stack size mismatch");

	cpu_test_reset();
}

Test(cpu_irq, topology_rejects_invalid_inputs_and_bootstrap_accessors_work) {
	const struct cpu_init_info init_info[] = {
		{
         .index           = 0u,
         .processor_id    = 1u,
         .arch_id         = 2u,
         .role            = CPU_ROLE_AP,
         .boot_stack_base = 0x200000u,
         .boot_stack_top  = 0x204000u,
		 },
	};

	cr_assert(!cpu_topology_init(NULL, 1u, 0u), "cpu_topology_init accepted NULL init_info");
	cr_assert(!cpu_topology_init(init_info, 0u, 0u), "cpu_topology_init accepted an empty topology");
	cr_assert(!cpu_topology_init(init_info, 1u, 1u), "cpu_topology_init accepted an out-of-range BSP index");
	cr_assert(!cpu_topology_init(init_info, 65u, 0u), "cpu_topology_init accepted too many CPUs");

	cpu_test_init_bound_bootstrap();

	cr_assert_eq(cpu_count(), 1u, "bootstrap topology should expose one CPU");
	cr_assert_not_null(cpu_current(), "cpu_current returned NULL after binding");
	cr_assert_eq(cpu_current(), cpu_bsp(), "cpu_current should match the bootstrap CPU");
	cr_assert_eq(cpu_index(), 0u, "bootstrap CPU index mismatch");
	cr_assert_eq(cpu_arch_id(), 0u, "hosted bootstrap arch id should come from the HAL mock");
	cr_assert(cpu_is_bsp(), "current CPU should be the BSP");
	cr_assert_eq(cpu_current()->boot_stack_base, 0x100000u, "bootstrap stack base mismatch");
	cr_assert_eq(cpu_current()->boot_stack_top, 0x104000u, "bootstrap stack top mismatch");
	cr_assert_null(cpu_by_index(1u), "cpu_by_index accepted an out-of-range index");

	cpu_test_reset();
}

Test(cpu, topology_rejects_descriptor_indices_that_do_not_match_stable_slots) {
	struct cpu_init_info descriptors[2] = {
		cpu_test_valid_topology[0],
		cpu_test_valid_topology[1],
	};

	descriptors[0].index = 1u;
	descriptors[1].index = 0u;
	cr_assert_not(cpu_topology_init(descriptors, 2u, 0u),
	              "topology must reject descriptors whose stable index does not match their slot");

	descriptors[0]       = cpu_test_valid_topology[0];
	descriptors[1]       = cpu_test_valid_topology[1];
	descriptors[1].index = 0u;
	cr_assert_not(cpu_topology_init(descriptors, 2u, 0u), "topology must reject duplicate stable CPU indices");

	descriptors[0]       = cpu_test_valid_topology[0];
	descriptors[1]       = cpu_test_valid_topology[1];
	descriptors[1].index = 64u;
	cr_assert_not(cpu_topology_init(descriptors, 2u, 0u), "topology must reject a stable index outside CPU storage");

	cr_assert(cpu_topology_init(cpu_test_valid_topology, 2u, 0u), "valid contiguous topology must remain accepted");
	cr_assert_eq(cpu_by_index(0u)->index, 0u, "slot zero must expose stable index zero");
	cr_assert_eq(cpu_by_index(1u)->index, 1u, "slot one must expose stable index one");

	cpu_test_reset();
}

Test(cpu, topology_reinitialization_resets_publication_state) {
	cr_assert(cpu_topology_init(cpu_test_valid_topology, 2u, 0u), "initial topology init failed");
	cr_assert(cpu_set_state(cpu_by_index(0u), CPU_STATE_ONLINE), "BSP online transition failed");
	cr_assert_eq(cpu_online_count(), 1u, "initial topology must publish the online BSP");

	cr_assert(cpu_topology_init(cpu_test_valid_topology, 2u, 0u), "topology reinitialization failed");
	cr_assert_eq(cpu_online_count(), 0u, "reinitialization must reset the published online count");
	cr_assert_eq(
		cpu_state_get(cpu_by_index(0u)), CPU_STATE_PRESENT, "reinitialization must reset CPU state to PRESENT");
	cr_assert_eq(
		cpu_state_get(cpu_by_index(1u)), CPU_STATE_PRESENT, "reinitialization must reset every CPU descriptor");

	cpu_test_reset();
}
