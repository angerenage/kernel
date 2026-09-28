#include <core/mm.h>
#include <criterion/criterion.h>
#include <firmware/acpi.h>
#include <kernel/boot.h>
#include <kernel/hardware/pci.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_ARENA_SIZE 0x4000u
#define TEST_PHYSICAL_BASE 0x1000u
#define TEST_RSDP_OFFSET 0x100u
#define TEST_ROOT_OFFSET 0x200u
#define TEST_FADT_DSDT_OFFSET 40u
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

static uint8_t                          test_arena[TEST_ARENA_SIZE];
static struct mem_range                 test_ranges[3];
static size_t                           test_range_count;
static struct kernel_boot_address_space test_address_space;
static size_t                           test_memmap_calls;

const struct mem_range* kernel_boot_memmap(size_t* out_count) {
	test_memmap_calls++;
	if (out_count != NULL) *out_count = test_range_count;
	return test_range_count == 0u ? NULL : test_ranges;
}

bool kernel_boot_address_space_get(struct kernel_boot_address_space* out) {
	if (out == NULL) return false;
	*out = test_address_space;
	return true;
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
	memset(test_arena, 0, sizeof(test_arena));
	memset(test_ranges, 0, sizeof(test_ranges));
	test_range_count = 1u;
	test_ranges[0]   = (struct mem_range){
		.base   = TEST_PHYSICAL_BASE,
		.length = TEST_ARENA_SIZE,
		.type   = MEM_RANGE_ACPI,
	};
	test_address_space = (struct kernel_boot_address_space){
		.direct_map_offset = (uintptr_t)test_arena - TEST_PHYSICAL_BASE,
	};
	test_memmap_calls = 0u;
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
	cr_assert(acpi_init(rsdp));

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
	cr_assert(acpi_init(rsdp));

	test_memmap_calls = 0u;
	cr_assert_eq(acpi_table_next("APIC", NULL), apic);
	cr_assert_eq(test_memmap_calls, 3u);
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
	cr_assert(acpi_init(rsdp));

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
	cr_assert(acpi_init(rsdp));

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
	cr_assert(acpi_init(rsdp));

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
	cr_assert_not(acpi_init(rsdp));
	test_checksum(rsdp, 20u, offsetof(struct test_rsdp, checksum));
	cr_assert(acpi_init(rsdp));
	cr_assert(acpi_init(NULL));
}

Test(acpi, rejects_invalid_rsdp_fields_and_hhdm_addresses) {
	struct test_rsdp* rsdp;

	test_reset();
	test_root(true, NULL, 0u);
	rsdp               = test_rsdp(true);
	rsdp->signature[0] = 'X';
	cr_assert_not(acpi_init(rsdp));

	rsdp         = test_rsdp(true);
	rsdp->length = sizeof(*rsdp) - 1u;
	cr_assert_not(acpi_init(rsdp));

	rsdp                    = test_rsdp(true);
	rsdp->extended_checksum = (uint8_t)(rsdp->extended_checksum + 1u);
	cr_assert_not(acpi_init(rsdp));

	rsdp         = test_rsdp(true);
	rsdp->length = 4097u;
	cr_assert_not(acpi_init(rsdp));

	rsdp               = test_rsdp(true);
	rsdp->xsdt_address = UINT64_MAX;
	test_checksum(rsdp, sizeof(*rsdp), offsetof(struct test_rsdp, extended_checksum));
	cr_assert_not(acpi_init(rsdp));

	cr_assert_not(acpi_init((const void*)(test_address_space.direct_map_offset - 1u)));
}

Test(acpi, rejects_invalid_root_signature_and_entry_alignment) {
	struct acpi_sdt_header* root;
	struct test_rsdp*       rsdp;

	test_reset();
	root = test_root(true, NULL, 0u);
	memcpy(root->signature, "RSDT", 4u);
	test_checksum(root, root->length, offsetof(struct acpi_sdt_header, checksum));
	rsdp = test_rsdp(true);
	cr_assert_not(acpi_init(rsdp));

	test_reset();
	test_table(TEST_ROOT_OFFSET, "XSDT", sizeof(*root) + 1u);
	rsdp = test_rsdp(true);
	cr_assert_not(acpi_init(rsdp));
}

Test(acpi, advertised_invalid_xsdt_does_not_fall_back_to_rsdt) {
	struct acpi_sdt_header* root;
	struct test_rsdp*       rsdp;

	test_reset();
	root = test_root(true, NULL, 0u);
	root->checksum++;
	rsdp = test_rsdp(true);
	cr_assert_not(acpi_init(rsdp));
}

Test(acpi, table_may_cross_adjacent_acpi_ranges) {
	const uintptr_t         entries[] = {test_physical(0x800u)};
	struct acpi_sdt_header* table;
	struct test_rsdp*       rsdp;

	test_reset();
	table = test_table(0x800u, "APIC", 64u);
	test_root(true, entries, 1u);
	rsdp             = test_rsdp(true);
	test_range_count = 2u;
	test_ranges[0]   = (struct mem_range){.base = TEST_PHYSICAL_BASE, .length = 0x820u, .type = MEM_RANGE_ACPI};
	test_ranges[1] =
		(struct mem_range){.base = test_physical(0x820u), .length = TEST_ARENA_SIZE - 0x820u, .type = MEM_RANGE_ACPI};
	cr_assert(acpi_init(rsdp));
	cr_assert_eq(acpi_table_next("APIC", NULL), table);
}

Test(acpi, table_may_not_cross_a_gap_between_acpi_ranges) {
	const uintptr_t   entries[] = {test_physical(0x800u)};
	struct test_rsdp* rsdp;
	acpi_cursor_t     cursor = ACPI_CURSOR_INIT;

	test_reset();
	test_table(0x800u, "APIC", 64u);
	test_root(true, entries, 1u);
	rsdp             = test_rsdp(true);
	test_range_count = 2u;
	test_ranges[0]   = (struct mem_range){.base = TEST_PHYSICAL_BASE, .length = 0x820u, .type = MEM_RANGE_ACPI};
	test_ranges[1] =
		(struct mem_range){.base = test_physical(0x821u), .length = TEST_ARENA_SIZE - 0x821u, .type = MEM_RANGE_ACPI};
	cr_assert(acpi_init(rsdp));
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
	cr_assert(acpi_init(rsdp));
	cr_assert_null(acpi_table_next("APIC", &cursor));
	cr_assert_eq(cursor, 1u);
}

Test(acpi, rejects_non_acpi_or_reserved_ranges) {
	struct test_rsdp* rsdp;

	test_reset();
	test_root(true, NULL, 0u);
	rsdp                = test_rsdp(true);
	test_ranges[0].type = MEM_RANGE_OTHER;
	cr_assert_not(acpi_init(rsdp));
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
	struct pci_controller controller;
	struct test_rsdp*     rsdp;

	test_reset();
	test_mcfg(0x800u, first, sizeof(first) / sizeof(first[0]));
	test_mcfg(0xa00u, second, sizeof(second) / sizeof(second[0]));
	test_table(0xc00u, "MCFG", sizeof(struct acpi_sdt_header) + 9u);
	test_root(true, entries, sizeof(entries) / sizeof(entries[0]));
	rsdp = test_rsdp(true);
	cr_assert(acpi_init(rsdp));

	cr_assert_eq(kernel_hardware_pci_count(), 2u);
	cr_assert(kernel_hardware_pci_get(0u, &controller));
	cr_assert_eq(controller.access, PCI_CONFIG_ACCESS_ECAM);
	cr_assert_eq(controller.register_address, 0xe0000000u);
	cr_assert_eq(controller.register_size, 128u * 1024u * 1024u);
	cr_assert_eq(controller.domain, 0u);
	cr_assert_eq(controller.start_bus, 0u);
	cr_assert_eq(controller.end_bus, 127u);
	cr_assert(kernel_hardware_pci_get(1u, &controller));
	cr_assert_eq(controller.register_address, 0xf8000000u);
	cr_assert_eq(controller.register_size, 128u * 1024u * 1024u);
	cr_assert_eq(controller.domain, 2u);
	cr_assert_eq(controller.start_bus, 128u);
	cr_assert_eq(controller.end_bus, 255u);
	cr_assert_not(kernel_hardware_pci_get(2u, &controller));
	cr_assert_not(kernel_hardware_pci_get(0u, NULL));
}
