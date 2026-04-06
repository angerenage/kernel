#include <core/cpu.h>
#include <kernel/cpu_boot.h>
#include <kernel/selftest.h>

static void kernel_selftest_cpu_topology_is_consistent(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();
	struct cpu*          bsp      = cpu_bsp();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");
	KERNEL_SELFTEST_ASSERT(ctx, topology->cpu_count > 0u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_count() == topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() >= 1u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() <= cpu_count());
	KERNEL_SELFTEST_ASSERT_MSG(ctx, bsp != NULL, "cpu_bsp returned NULL");
	KERNEL_SELFTEST_ASSERT(ctx, topology->bsp_index < topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, bsp == &topology->cpus[topology->bsp_index]);
	KERNEL_SELFTEST_ASSERT(ctx, bsp->role == CPU_ROLE_BSP);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_state_get(bsp) == CPU_STATE_ONLINE);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_current() == bsp);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_is_bsp());
	KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(bsp));
	if (cpu_count() > 1u) {
		KERNEL_SELFTEST_ASSERT_MSG(ctx, cpu_online_count() == cpu_count(), "not all discovered CPUs reached ONLINE");
	}
}

static void kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");

	for (size_t i = 0; i < topology->cpu_count; i++) {
		const struct cpu* cpu = &topology->cpus[i];

		KERNEL_SELFTEST_ASSERT(ctx, cpu->index == i);
		KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(cpu));

		for (size_t j = i + 1; j < topology->cpu_count; j++) {
			KERNEL_SELFTEST_ASSERT_MSG(
				ctx, cpu->arch_id != topology->cpus[j].arch_id, "duplicate cpu arch_id discovered during boot");
		}
	}
}

static const struct kernel_selftest_case kernel_cpu_selftests[] = {
	{
     .name = "topology_is_consistent",
     .run  = kernel_selftest_cpu_topology_is_consistent,
	 },
	{
     .name = "ids_are_unique_and_bindings_succeeded",
     .run  = kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded,
	 },
};

const struct kernel_selftest_suite kernel_cpu_selftest_suite = {
	.name       = "cpu",
	.cases      = kernel_cpu_selftests,
	.case_count = sizeof(kernel_cpu_selftests) / sizeof(kernel_cpu_selftests[0]),
};
