#include <base/device.h>
#include <base/hardware/pci.h>
#include <base/hardware/ps2.h>
#include <boot/info.h>
#include <core/mm.h>
#include <criterion/criterion.h>
#include <firmware/acpi.h>
#include <kernel/device.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../kernel/src/hardware/pci.h"
#include "../kernel/src/hardware/ps2.h"
#include "heap/test_support.h"

#define TEST_ARENA_SIZE 0x4000u
#define TEST_PHYSICAL_BASE 0x1000u
#define TEST_RSDP_OFFSET 0x100u
#define TEST_ROOT_OFFSET 0x200u
#define TEST_FADT_DSDT_OFFSET 40u
#define TEST_FADT_IAPC_BOOT_ARCH_OFFSET 109u
#define TEST_FADT_IAPC_BOOT_ARCH_8042 (1u << 1u)
#define TEST_FADT_X_DSDT_OFFSET 140u

struct test_rsdp {
	char     signature[8];
	uint8_t  checksum;
	char     oem_id[6];
	uint8_t  revision;
	uint32_t rsdt_address;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct test_mcfg_allocation {
	uint64_t address;
	uint16_t segment_group;
	uint8_t  start_bus;
	uint8_t  end_bus;
	uint32_t reserved;
} __attribute__((packed));

static uint8_t          test_arena[TEST_ARENA_SIZE];
static uint8_t          test_heap[64u * 1024u] __attribute__((aligned(4096)));
static struct mem_range test_ranges[3];
static size_t           test_range_count;
static struct boot_info test_boot_info;

static bool test_acpi_init(const void* rsdp) {
	test_boot_info.rsdp_address = (uintptr_t)rsdp;
	return acpi_init(&test_boot_info);
}

static uintptr_t test_physical(size_t offset) {
	return TEST_PHYSICAL_BASE + offset;
}

static void test_checksum(void* data, size_t size, size_t checksum_offset) {
	uint8_t* bytes = data;
	uint8_t  sum   = 0u;

	bytes[checksum_offset] = 0u;
	for (size_t index = 0u; index < size; index++) sum = (uint8_t)(sum + bytes[index]);
	bytes[checksum_offset] = (uint8_t)(0u - sum);
}

static void test_reset(void) {
	kernel_device_reset_for_test();
	if (!heap_is_initialized()) init_test_heap(test_heap, sizeof(test_heap));
	memset(test_arena, 0, sizeof(test_arena));
	memset(test_ranges, 0, sizeof(test_ranges));
	test_range_count = 1u;
	test_ranges[0]   = (struct mem_range){
		.base   = TEST_PHYSICAL_BASE,
		.length = TEST_ARENA_SIZE,
		.type   = MEM_RANGE_ACPI,
	};
	test_boot_info = (struct boot_info){
		.memory_map        = test_ranges,
		.memory_map_count  = test_range_count,
		.direct_map_offset = (uintptr_t)test_arena - TEST_PHYSICAL_BASE,
	};
}

static size_t test_pci_controllers(struct pci_controller* controllers, size_t capacity) {
	size_t count;
	size_t returned;

	cr_assert(kernel_device_register_pci());
	kernel_device_freeze();
	cr_assert(kernel_device_count(KERNEL_DEVICE_TYPE_PCI, &count));
	cr_assert(kernel_device_list(KERNEL_DEVICE_TYPE_PCI, 0u, capacity, sizeof(*controllers), controllers, &returned));
	cr_assert_eq(returned, count < capacity ? count : capacity);
	return count;
}

static size_t test_ps2_controllers(struct ps2_controller* controllers, size_t capacity) {
	size_t count;
	size_t returned;

	cr_assert(kernel_device_register_ps2_controllers());
	kernel_device_freeze();
	cr_assert(kernel_device_count(KERNEL_DEVICE_TYPE_PS2_CONTROLLER, &count));
	cr_assert(kernel_device_list(
		KERNEL_DEVICE_TYPE_PS2_CONTROLLER, 0u, capacity, sizeof(*controllers), controllers, &returned));
	cr_assert_eq(returned, count < capacity ? count : capacity);
	return count;
}

static struct acpi_sdt_header* test_table(size_t offset, const char signature[4], size_t size) {
	struct acpi_sdt_header* table = (struct acpi_sdt_header*)(test_arena + offset);

	memset(table, 0, size);
	memcpy(table->signature, signature, 4u);
	table->length   = (uint32_t)size;
	table->revision = 1u;
	test_checksum(table, size, offsetof(struct acpi_sdt_header, checksum));
	return table;
}

static struct acpi_sdt_header* test_root(bool xsdt, const uintptr_t* entries, size_t entry_count) {
	size_t                  entry_size = xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
	size_t                  size       = sizeof(struct acpi_sdt_header) + entry_count * entry_size;
	struct acpi_sdt_header* root       = test_table(TEST_ROOT_OFFSET, xsdt ? "XSDT" : "RSDT", size);
	uint8_t*                output     = (uint8_t*)root + sizeof(*root);

	for (size_t index = 0u; index < entry_count; index++) {
		if (xsdt) {
			uint64_t address = entries[index];
			memcpy(output + index * entry_size, &address, sizeof(address));
		}
		else {
			uint32_t address = (uint32_t)entries[index];
			memcpy(output + index * entry_size, &address, sizeof(address));
		}
	}
	test_checksum(root, size, offsetof(struct acpi_sdt_header, checksum));
	return root;
}

static struct test_rsdp* test_rsdp(bool xsdt) {
	struct test_rsdp* rsdp = (struct test_rsdp*)(test_arena + TEST_RSDP_OFFSET);

	memset(rsdp, 0, sizeof(*rsdp));
	memcpy(rsdp->signature, "RSD PTR ", 8u);
	memcpy(rsdp->oem_id, "CODEX ", 6u);
	rsdp->revision     = xsdt ? 2u : 0u;
	rsdp->rsdt_address = (uint32_t)test_physical(TEST_ROOT_OFFSET);
	if (xsdt) {
		rsdp->length       = sizeof(*rsdp);
		rsdp->xsdt_address = test_physical(TEST_ROOT_OFFSET);
	}
	test_checksum(rsdp, 20u, offsetof(struct test_rsdp, checksum));
	if (xsdt) test_checksum(rsdp, sizeof(*rsdp), offsetof(struct test_rsdp, extended_checksum));
	return rsdp;
}

static struct acpi_sdt_header* test_fadt(size_t offset, size_t size, uint32_t dsdt, uint64_t x_dsdt) {
	struct acpi_sdt_header* fadt  = test_table(offset, "FACP", size);
	uint8_t*                bytes = (uint8_t*)fadt;

	if (size >= TEST_FADT_DSDT_OFFSET + sizeof(dsdt)) memcpy(bytes + TEST_FADT_DSDT_OFFSET, &dsdt, sizeof(dsdt));
	if (size >= TEST_FADT_X_DSDT_OFFSET + sizeof(x_dsdt))
		memcpy(bytes + TEST_FADT_X_DSDT_OFFSET, &x_dsdt, sizeof(x_dsdt));
	test_checksum(fadt, size, offsetof(struct acpi_sdt_header, checksum));
	return fadt;
}

static struct acpi_sdt_header* test_fadt_boot_arch(size_t offset, size_t size, uint16_t boot_arch) {
	struct acpi_sdt_header* fadt = test_table(offset, "FACP", size);

	fadt->revision = 2u;
	if (size >= TEST_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(boot_arch))
		memcpy((uint8_t*)fadt + TEST_FADT_IAPC_BOOT_ARCH_OFFSET, &boot_arch, sizeof(boot_arch));
	test_checksum(fadt, size, offsetof(struct acpi_sdt_header, checksum));
	return fadt;
}

static struct acpi_sdt_header* test_mcfg(size_t offset, const struct test_mcfg_allocation* allocations,
                                         size_t allocation_count) {
	size_t                  size  = sizeof(struct acpi_sdt_header) + 8u + allocation_count * sizeof(*allocations);
	struct acpi_sdt_header* table = test_table(offset, "MCFG", size);

	if (allocation_count != 0u)
		memcpy((uint8_t*)table + sizeof(*table) + 8u, allocations, allocation_count * sizeof(*allocations));
	test_checksum(table, size, offsetof(struct acpi_sdt_header, checksum));
	return table;
}

Test(acpi, cursor_iteration_skips_invalid_tables_and_preserves_duplicates) {
	const uintptr_t entries[] = {
		test_physical(0x400u),
		test_physical(0x500u),
		test_physical(0x400u),
		test_physical(0x600u),
	};
	struct acpi_sdt_header*       apic;
	struct acpi_sdt_header*       invalid;
	struct acpi_sdt_header*       facp;
	const struct acpi_sdt_header* found;
	struct test_rsdp*             rsdp;
	acpi_cursor_t                 cursor = ACPI_CURSOR_INIT;

	test_reset();
	apic    = test_table(0x400u, "APIC", sizeof(*apic));
	invalid = test_table(0x500u, "APIC", sizeof(*invalid));
	facp    = test_table(0x600u, "FACP", sizeof(*facp));
	invalid->checksum++;
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	found = acpi_table_next("APIC", &cursor);
	cr_assert_eq(found, apic);
	cr_assert_eq(cursor, 1u);
	found = acpi_table_next("APIC", &cursor);
	cr_assert_eq(found, apic);
	cr_assert_eq(cursor, 3u);
	cr_assert_eq(acpi_table_next("APIC", NULL), apic);
	cr_assert_eq(acpi_table_next("APIC", NULL), apic);
	cr_assert_eq(acpi_table_next("FACP", &cursor), facp);
	cr_assert_eq(cursor, 4u);
	cr_assert_null(acpi_table_next("FACP", &cursor));
	cr_assert_eq(cursor, 4u);
	cursor = ACPI_CURSOR_INIT;
	cr_assert_eq(acpi_table_next("APIC", &cursor), apic);
	cr_assert_eq(cursor, 1u);

	cursor = 2u;
	cr_assert_null(acpi_table_next("XSDT", &cursor));
	cr_assert_eq(cursor, 2u);

	cursor = SIZE_MAX;
	cr_assert_null(acpi_table_next("APIC", &cursor));
	cr_assert_eq(cursor, 4u);
}

Test(acpi, filters_signatures_before_validating_table_contents) {
	const uintptr_t         entries[] = {test_physical(0x400u), test_physical(0x500u)};
	struct acpi_sdt_header* unrelated;
	struct acpi_sdt_header* apic;
	struct test_rsdp*       rsdp;

	test_reset();
	unrelated = test_table(0x400u, "FACP", sizeof(*unrelated));
	unrelated->checksum++;
	apic = test_table(0x500u, "APIC", sizeof(*apic));
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(acpi_table_next("APIC", NULL), apic);
}

Test(acpi, resolves_legacy_dsdt_and_keeps_fadt_accessible) {
	const uintptr_t         entries[] = {test_physical(0x400u)};
	struct acpi_sdt_header* fadt;
	struct acpi_sdt_header* dsdt;
	struct test_rsdp*       rsdp;

	test_reset();
	dsdt = test_table(0x800u, "DSDT", 64u);
	fadt = test_fadt(0x400u, TEST_FADT_DSDT_OFFSET + sizeof(uint32_t), (uint32_t)test_physical(0x800u), 0u);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert_null(acpi_dsdt());
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(acpi_dsdt(), dsdt);
	cr_assert_eq(acpi_table_next("FACP", NULL), fadt);
	cr_assert_null(acpi_table_next("DSDT", NULL));
}

Test(acpi, prefers_extended_dsdt_address) {
	const uintptr_t         entries[] = {test_physical(0x400u)};
	struct acpi_sdt_header* legacy;
	struct acpi_sdt_header* extended;
	struct test_rsdp*       rsdp;

	test_reset();
	legacy   = test_table(0x800u, "DSDT", 64u);
	extended = test_table(0xa00u, "DSDT", 64u);
	test_fadt(
		0x400u, TEST_FADT_X_DSDT_OFFSET + sizeof(uint64_t), (uint32_t)test_physical(0x800u), test_physical(0xa00u));
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_neq(legacy, extended);
	cr_assert_eq(acpi_dsdt(), extended);
}

Test(acpi, rejects_invalid_extended_dsdt_without_hiding_fadt) {
	const uintptr_t         entries[] = {test_physical(0x400u)};
	struct acpi_sdt_header* fadt;
	struct acpi_sdt_header* extended;
	struct test_rsdp*       rsdp;

	test_reset();
	test_table(0x800u, "DSDT", 64u);
	extended = test_table(0xa00u, "DSDT", 64u);
	extended->checksum++;
	fadt = test_fadt(
		0x400u, TEST_FADT_X_DSDT_OFFSET + sizeof(uint64_t), (uint32_t)test_physical(0x800u), test_physical(0xa00u));
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_null(acpi_dsdt());
	cr_assert_eq(acpi_table_next("FACP", NULL), fadt);
}

Test(acpi, unavailable_provider_leaves_cursor_unchanged) {
	acpi_cursor_t cursor = 2u;

	cr_assert_null(acpi_table_next("APIC", &cursor));
	cr_assert_eq(cursor, 2u);
}

Test(acpi, failed_initialization_can_retry_and_success_is_immutable) {
	struct test_rsdp* rsdp;

	test_reset();
	test_root(false, NULL, 0u);
	rsdp = test_rsdp(false);
	rsdp->checksum++;
	cr_assert_not(test_acpi_init(rsdp));
	test_checksum(rsdp, 20u, offsetof(struct test_rsdp, checksum));
	cr_assert(test_acpi_init(rsdp));
	cr_assert(test_acpi_init(NULL));
}

Test(acpi, rejects_invalid_rsdp_fields_and_hhdm_addresses) {
	struct test_rsdp* rsdp;

	test_reset();
	test_root(true, NULL, 0u);
	rsdp               = test_rsdp(true);
	rsdp->signature[0] = 'X';
	cr_assert_not(test_acpi_init(rsdp));

	rsdp         = test_rsdp(true);
	rsdp->length = sizeof(*rsdp) - 1u;
	cr_assert_not(test_acpi_init(rsdp));

	rsdp                    = test_rsdp(true);
	rsdp->extended_checksum = (uint8_t)(rsdp->extended_checksum + 1u);
	cr_assert_not(test_acpi_init(rsdp));

	rsdp         = test_rsdp(true);
	rsdp->length = 4097u;
	cr_assert_not(test_acpi_init(rsdp));

	rsdp               = test_rsdp(true);
	rsdp->xsdt_address = UINT64_MAX;
	test_checksum(rsdp, sizeof(*rsdp), offsetof(struct test_rsdp, extended_checksum));
	cr_assert_not(test_acpi_init(rsdp));

	cr_assert_not(test_acpi_init((const void*)(test_boot_info.direct_map_offset - 1u)));
}

Test(acpi, rejects_invalid_root_signature_and_entry_alignment) {
	struct acpi_sdt_header* root;
	struct test_rsdp*       rsdp;

	test_reset();
	root = test_root(true, NULL, 0u);
	memcpy(root->signature, "RSDT", 4u);
	test_checksum(root, root->length, offsetof(struct acpi_sdt_header, checksum));
	rsdp = test_rsdp(true);
	cr_assert_not(test_acpi_init(rsdp));

	test_reset();
	test_table(TEST_ROOT_OFFSET, "XSDT", sizeof(*root) + 1u);
	rsdp = test_rsdp(true);
	cr_assert_not(test_acpi_init(rsdp));
}

Test(acpi, advertised_invalid_xsdt_does_not_fall_back_to_rsdt) {
	struct acpi_sdt_header* root;
	struct test_rsdp*       rsdp;

	test_reset();
	root = test_root(true, NULL, 0u);
	root->checksum++;
	rsdp = test_rsdp(true);
	cr_assert_not(test_acpi_init(rsdp));
}

Test(acpi, table_may_cross_adjacent_acpi_ranges) {
	const uintptr_t         entries[] = {test_physical(0x800u)};
	struct acpi_sdt_header* table;
	struct test_rsdp*       rsdp;

	test_reset();
	table = test_table(0x800u, "APIC", 64u);
	test_root(true, entries, 1u);
	rsdp                            = test_rsdp(true);
	test_range_count                = 2u;
	test_boot_info.memory_map_count = test_range_count;
	test_ranges[0] = (struct mem_range){.base = TEST_PHYSICAL_BASE, .length = 0x820u, .type = MEM_RANGE_ACPI};
	test_ranges[1] =
		(struct mem_range){.base = test_physical(0x820u), .length = TEST_ARENA_SIZE - 0x820u, .type = MEM_RANGE_ACPI};
	cr_assert(test_acpi_init(rsdp));
	cr_assert_eq(acpi_table_next("APIC", NULL), table);
}

Test(acpi, table_may_not_cross_a_gap_between_acpi_ranges) {
	const uintptr_t   entries[] = {test_physical(0x800u)};
	struct test_rsdp* rsdp;
	acpi_cursor_t     cursor = ACPI_CURSOR_INIT;

	test_reset();
	test_table(0x800u, "APIC", 64u);
	test_root(true, entries, 1u);
	rsdp                            = test_rsdp(true);
	test_range_count                = 2u;
	test_boot_info.memory_map_count = test_range_count;
	test_ranges[0] = (struct mem_range){.base = TEST_PHYSICAL_BASE, .length = 0x820u, .type = MEM_RANGE_ACPI};
	test_ranges[1] =
		(struct mem_range){.base = test_physical(0x821u), .length = TEST_ARENA_SIZE - 0x821u, .type = MEM_RANGE_ACPI};
	cr_assert(test_acpi_init(rsdp));
	cr_assert_null(acpi_table_next("APIC", &cursor));
	cr_assert_eq(cursor, 1u);
}

Test(acpi, rejects_oversized_children) {
	const uintptr_t         entries[] = {test_physical(0x800u)};
	struct acpi_sdt_header* table;
	struct test_rsdp*       rsdp;
	acpi_cursor_t           cursor = ACPI_CURSOR_INIT;

	test_reset();
	table         = test_table(0x800u, "APIC", sizeof(*table));
	table->length = 16u * 1024u * 1024u + 1u;
	test_root(true, entries, 1u);
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));
	cr_assert_null(acpi_table_next("APIC", &cursor));
	cr_assert_eq(cursor, 1u);
}

Test(acpi, rejects_non_acpi_or_reserved_ranges) {
	struct test_rsdp* rsdp;

	test_reset();
	test_root(true, NULL, 0u);
	rsdp                = test_rsdp(true);
	test_ranges[0].type = MEM_RANGE_OTHER;
	cr_assert_not(test_acpi_init(rsdp));
}

Test(acpi, pci_controllers_are_normalized_from_all_valid_mcfg_allocations) {
	const uintptr_t entries[] = {
		test_physical(0x800u),
		test_physical(0xa00u),
		test_physical(0xc00u),
	};
	const struct test_mcfg_allocation first[] = {
		{.address = 0xe0000000u, .segment_group = 0u, .start_bus = 0u, .end_bus = 127u},
		{		 .address = 0u, .segment_group = 1u, .start_bus = 0u, .end_bus = 255u},
	};
	const struct test_mcfg_allocation second[] = {
		{.address = 0xf0000000u, .segment_group = 2u, .start_bus = 128u, .end_bus = 255u},
	};
	struct pci_controller controllers[3];
	struct test_rsdp*     rsdp;

	test_reset();
	test_mcfg(0x800u, first, sizeof(first) / sizeof(first[0]));
	test_mcfg(0xa00u, second, sizeof(second) / sizeof(second[0]));
	test_table(0xc00u, "MCFG", sizeof(struct acpi_sdt_header) + 9u);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(test_pci_controllers(controllers, 3u), 2u);
	cr_assert_eq(controllers[0].access, PCI_CONFIG_ACCESS_ECAM);
	cr_assert_eq(controllers[0].register_address, 0xe0000000u);
	cr_assert_eq(controllers[0].register_size, 128u * 1024u * 1024u);
	cr_assert_eq(controllers[0].domain, 0u);
	cr_assert_eq(controllers[0].start_bus, 0u);
	cr_assert_eq(controllers[0].end_bus, 127u);
	cr_assert_eq(controllers[1].register_address, 0xf8000000u);
	cr_assert_eq(controllers[1].register_size, 128u * 1024u * 1024u);
	cr_assert_eq(controllers[1].domain, 2u);
	cr_assert_eq(controllers[1].start_bus, 128u);
	cr_assert_eq(controllers[1].end_bus, 255u);
}

Test(acpi, ps2_controller_is_published_from_i8042_boot_flag) {
	const uintptr_t       entries[] = {test_physical(0x400u)};
	struct ps2_controller controllers[1];
	struct test_rsdp*     rsdp;

	test_reset();
	test_fadt_boot_arch(0x400u, TEST_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(uint16_t), TEST_FADT_IAPC_BOOT_ARCH_8042);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(test_ps2_controllers(controllers, 1u), 1u);
	cr_assert_eq(controllers[0].interface, PS2_CONTROLLER_INTERFACE_I8042_IO_PORT);
	cr_assert_eq(controllers[0].reserved, 0u);
	cr_assert_eq(controllers[0].access.i8042_io_port.data_port, 0x60u);
	cr_assert_eq(controllers[0].access.i8042_io_port.status_command_port, 0x64u);
}

Test(acpi, ps2_controller_is_absent_when_i8042_boot_flag_is_clear) {
	const uintptr_t       entries[]      = {test_physical(0x400u)};
	struct ps2_controller controllers[1] = {{.interface = PS2_CONTROLLER_INTERFACE_COUNT}};
	struct test_rsdp*     rsdp;

	test_reset();
	test_fadt_boot_arch(0x400u, TEST_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(uint16_t), 0u);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(test_ps2_controllers(controllers, 1u), 0u);
	cr_assert_eq(controllers[0].interface, PS2_CONTROLLER_INTERFACE_COUNT);
}

Test(acpi, ps2_controller_is_absent_when_fadt_omits_boot_flags) {
	const uintptr_t       entries[] = {test_physical(0x400u)};
	struct ps2_controller controllers[1];
	struct test_rsdp*     rsdp;

	test_reset();
	test_fadt_boot_arch(0x400u, TEST_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(uint8_t), UINT16_MAX);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(test_ps2_controllers(controllers, 1u), 0u);
}

Test(acpi, ps2_controller_is_absent_when_fadt_revision_predates_boot_flags) {
	const uintptr_t         entries[] = {test_physical(0x400u)};
	struct ps2_controller   controllers[1];
	struct acpi_sdt_header* fadt;
	struct test_rsdp*       rsdp;

	test_reset();
	fadt =
		test_fadt_boot_arch(0x400u, TEST_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(uint16_t), TEST_FADT_IAPC_BOOT_ARCH_8042);
	fadt->revision = 1u;
	test_checksum(fadt, fadt->length, offsetof(struct acpi_sdt_header, checksum));
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(test_acpi_init(rsdp));

	cr_assert_eq(test_ps2_controllers(controllers, 1u), 0u);
}
