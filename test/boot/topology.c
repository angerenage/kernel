#include <stdint.h>
#include <string.h>

#include <core/cpu.h>

#include <criterion/criterion.h>

#include "test_support.h"

Test(boot_topology, valid_mp_response_selects_the_reported_bsp_slot) {
	struct limine_mp_info cpu0 = {.processor_id = 10u, .lapic_id = 0x31u};
	struct limine_mp_info cpu1 = {.processor_id = 11u, .lapic_id = 0x44u};
	struct limine_mp_info* cpus[] = {&cpu0, &cpu1};
	struct cpu_init_info init[2];
	size_t count = 0u;
	size_t bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x44u);
	cr_assert(boot_init());

	memset(init, 0, sizeof(init));
	cr_assert(boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index));
	cr_assert_eq(count, 2u);
	cr_assert_eq(bsp_index, 1u);
	cr_assert_eq(init[0].role, CPU_ROLE_AP);
	cr_assert_eq(init[1].role, CPU_ROLE_BSP);
	cr_assert_eq(init[1].boot_stack_base, 0x8000u);
	cr_assert_eq(init[1].boot_stack_top, 0xa000u);
}

static void boot_test_ap_entry(size_t cpu_index, void* arg) {
	(void)cpu_index;
	(void)arg;
}

Test(boot_topology, ap_launch_publishes_the_assigned_stack) {
	struct limine_mp_info bsp = {.processor_id = 20u, .lapic_id = 0x55u};
	struct limine_mp_info ap = {.processor_id = 21u, .lapic_id = 0x56u};
	struct limine_mp_info* cpus[] = {&bsp, &ap};
	struct cpu_init_info init[2];
	size_t count = 0u;
	size_t bsp_index = SIZE_MAX;
	const uintptr_t stack_top = 0x20000u;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x55u);
	cr_assert(boot_init());
	cr_assert(boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index));
	cr_assert(boot_cpu_start(1u, boot_test_ap_entry, &ap, stack_top));
	cr_assert_eq(ap.goto_address, boot_test_mp_entry());
	cr_assert_eq(boot_test_launch_entry(1u), boot_test_ap_entry);
	cr_assert_eq(boot_test_launch_arg(1u), &ap);
	cr_assert_eq(boot_test_launch_index(1u), 1u);
	cr_assert_eq(boot_test_launch_stack_top(1u), stack_top);
	cr_assert_neq(ap.extra_argument, 0u);
	cr_assert_not(boot_cpu_start(1u, boot_test_ap_entry, &ap, stack_top + 1u));
}

Test(boot_topology, rejects_null_cpu_entries) {
	struct limine_mp_info bsp = {.processor_id = 20u, .lapic_id = 0x55u};
	struct limine_mp_info* cpus[] = {&bsp, NULL};
	struct cpu_init_info init[2];
	size_t count = 0u;
	size_t bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x55u);
	cr_assert(boot_init());

	cr_assert_not(
			boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index), "NULL MP descriptors must be rejected");
}

Test(boot_topology, rejects_mp_response_without_the_reported_bsp) {
	struct limine_mp_info cpu0 = {.processor_id = 30u, .lapic_id = 0x61u};
	struct limine_mp_info cpu1 = {.processor_id = 31u, .lapic_id = 0x62u};
	struct limine_mp_info* cpus[] = {&cpu0, &cpu1};
	struct cpu_init_info init[2];
	size_t count = 0u;
	size_t bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x7fu);
	cr_assert(boot_init());

	cr_assert_not(boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index),
			"missing BSP must not silently relabel slot zero");
}
