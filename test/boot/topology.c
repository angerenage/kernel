#include <core/cpu.h>
#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"

Test(boot_topology, valid_mp_response_selects_the_reported_bsp_slot) {
	struct LIMINE_MP(info) cpu0     = {.processor_id = 10u, .lapic_id = 0x31u};
	struct LIMINE_MP(info) cpu1     = {.processor_id = 11u, .lapic_id = 0x44u};
	struct LIMINE_MP(info) * cpus[] = {&cpu0, &cpu1};
	struct cpu_init_info init[2];
	size_t               count     = 0u;
	size_t               bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x44u);
	cr_assert(kernel_boot_init());

	memset(init, 0, sizeof(init));
	cr_assert(kernel_boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index));
	cr_assert_eq(count, 2u);
	cr_assert_eq(bsp_index, 1u);
	cr_assert_eq(init[0].role, CPU_ROLE_AP);
	cr_assert_eq(init[1].role, CPU_ROLE_BSP);
	cr_assert_eq(init[1].boot_stack_base, 0x8000u);
	cr_assert_eq(init[1].boot_stack_top, 0xa000u);
}

Test(boot_topology, rejects_null_cpu_entries) {
	struct LIMINE_MP(info) bsp      = {.processor_id = 20u, .lapic_id = 0x55u};
	struct LIMINE_MP(info) * cpus[] = {&bsp, NULL};
	struct cpu_init_info init[2];
	size_t               count     = 0u;
	size_t               bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x55u);
	cr_assert(kernel_boot_init());

	cr_assert_not(kernel_boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index),
	              "NULL MP descriptors must be rejected");
}

Test(boot_topology, rejects_mp_response_without_the_reported_bsp) {
	struct LIMINE_MP(info) cpu0     = {.processor_id = 30u, .lapic_id = 0x61u};
	struct LIMINE_MP(info) cpu1     = {.processor_id = 31u, .lapic_id = 0x62u};
	struct LIMINE_MP(info) * cpus[] = {&cpu0, &cpu1};
	struct cpu_init_info init[2];
	size_t               count     = 0u;
	size_t               bsp_index = SIZE_MAX;

	boot_test_configure_valid_base();
	boot_test_configure_mp(cpus, 2u, 0x7fu);
	cr_assert(kernel_boot_init());

	cr_assert_not(kernel_boot_cpu_topology(init, 2u, 0x8000u, 0xa000u, &count, &bsp_index),
	              "missing BSP must not silently relabel slot zero");
}
