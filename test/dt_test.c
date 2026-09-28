#include <core/mm.h>
#include <criterion/criterion.h>
#include <firmware/dt.h>
#include <firmware/dt/device.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_ARENA_SIZE 0x2000u
#define TEST_PHYSICAL_BASE 0x1000u
#define TEST_RESERVATIONS_OFFSET 0x40u
#define TEST_STRUCTURE_OFFSET 0x100u
#define TEST_STRINGS_OFFSET 0x1000u
#define TEST_TOTAL_SIZE 0x1800u

static uint8_t                          test_arena[TEST_ARENA_SIZE] __attribute__((aligned(8)));
static struct mem_range                 test_ranges[3];
static size_t                           test_range_count;
static struct kernel_boot_address_space test_address_space;
static size_t                           test_structure_cursor;
static size_t                           test_strings_cursor;

const struct mem_range* kernel_boot_memmap(size_t* out_count) {
	if (out_count != NULL) *out_count = test_range_count;
	return test_range_count == 0u ? NULL : test_ranges;
}

bool kernel_boot_address_space_get(struct kernel_boot_address_space* out) {
	if (out == NULL) return false;
	*out = test_address_space;
	return true;
}

static void test_write_u32(uint8_t* output, uint32_t value) {
	output[0] = (uint8_t)(value >> 24u);
	output[1] = (uint8_t)(value >> 16u);
	output[2] = (uint8_t)(value >> 8u);
	output[3] = (uint8_t)value;
}

static void test_reset(void) {
	memset(test_arena, 0, sizeof(test_arena));
	memset(test_ranges, 0, sizeof(test_ranges));
	test_range_count = 1u;
	test_ranges[0]   = (struct mem_range){
		.base   = TEST_PHYSICAL_BASE,
		.length = TEST_ARENA_SIZE,
		.type   = MEM_RANGE_BOOTLOADER_RECLAIMABLE,
	};
	test_address_space = (struct kernel_boot_address_space){
		.direct_map_offset = (uintptr_t)test_arena - TEST_PHYSICAL_BASE,
	};
	test_structure_cursor = 0u;
	test_strings_cursor   = 0u;
}

static size_t test_string(const char* value) {
	size_t offset = test_strings_cursor;
	size_t size   = strlen(value) + 1u;

	memcpy(test_arena + TEST_STRINGS_OFFSET + offset, value, size);
	test_strings_cursor += size;
	return offset;
}

static void test_token(uint32_t token) {
	test_write_u32(test_arena + TEST_STRUCTURE_OFFSET + test_structure_cursor, token);
	test_structure_cursor += 4u;
}

static void test_begin_node(const char* name) {
	size_t size = strlen(name) + 1u;

	test_token(1u);
	memcpy(test_arena + TEST_STRUCTURE_OFFSET + test_structure_cursor, name, size);
	test_structure_cursor = (test_structure_cursor + size + 3u) & ~(size_t)3u;
}

static void test_end_node(void) {
	test_token(2u);
}

static void test_property(uint32_t version, size_t name_offset, const void* data, size_t size) {
	test_token(3u);
	test_write_u32(test_arena + TEST_STRUCTURE_OFFSET + test_structure_cursor, (uint32_t)size);
	test_write_u32(test_arena + TEST_STRUCTURE_OFFSET + test_structure_cursor + 4u, (uint32_t)name_offset);
	test_structure_cursor += 8u;
	if (version < 16u && size >= 8u && ((TEST_STRUCTURE_OFFSET + test_structure_cursor) & 7u) != 0u)
		test_structure_cursor += 4u;
	if (size != 0u) memcpy(test_arena + TEST_STRUCTURE_OFFSET + test_structure_cursor, data, size);
	test_structure_cursor = (test_structure_cursor + size + 3u) & ~(size_t)3u;
}

static void test_header(uint32_t version) {
	test_write_u32(test_arena, 0xd00dfeedu);
	test_write_u32(test_arena + 4u, TEST_TOTAL_SIZE);
	test_write_u32(test_arena + 8u, TEST_STRUCTURE_OFFSET);
	test_write_u32(test_arena + 12u, TEST_STRINGS_OFFSET);
	test_write_u32(test_arena + 16u, TEST_RESERVATIONS_OFFSET);
	test_write_u32(test_arena + 20u, version);
	test_write_u32(test_arena + 24u, version < 16u ? version : 16u);
	if (version >= 3u) test_write_u32(test_arena + 32u, (uint32_t)test_strings_cursor);
	if (version >= 17u) test_write_u32(test_arena + 36u, (uint32_t)test_structure_cursor);
}

static void test_tree(uint32_t version) {
	static const uint8_t compatible[] = "vendor,soc\0vendor,fallback\0";
	static const uint8_t disabled[]   = "disabled\0";
	static const uint8_t cells[]      = {0x12u, 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xdeu, 0xf0u};
	static const uint8_t phandle[]    = {0u, 0u, 0u, 7u};
	size_t               compatible_name;
	size_t               status_name;
	size_t               cells_name;
	size_t               phandle_name;
	size_t               empty_name;

	test_reset();
	compatible_name = test_string("compatible");
	status_name     = test_string("status");
	cells_name      = test_string("cells");
	phandle_name    = test_string("phandle");
	empty_name      = test_string("empty");
	test_begin_node("");
	test_begin_node("soc");
	test_property(version, compatible_name, compatible, sizeof(compatible));
	test_property(version, cells_name, cells, sizeof(cells));
	test_property(version, phandle_name, phandle, sizeof(phandle));
	test_begin_node("child");
	test_property(version, empty_name, NULL, 0u);
	test_end_node();
	test_end_node();
	test_begin_node("off");
	test_property(version, compatible_name, compatible, sizeof(compatible));
	test_property(version, status_name, disabled, sizeof(disabled));
	test_end_node();
	test_end_node();
	test_token(9u);
	test_header(version);
}

static void test_version(uint32_t version) {
	struct dt_property cells;
	struct dt_node     root;
	struct dt_node     soc;
	uint64_t           value;

	test_tree(version);
	cr_assert(dt_init(test_arena));
	root = dt_root();
	cr_assert(dt_node_valid(root));
	soc = dt_node_child(root);
	cr_assert(dt_node_valid(soc));
	cr_assert_str_eq(dt_node_name(soc), "soc");
	cr_assert(dt_node_property(soc, "cells", &cells));
	if (version < 16u) cr_assert_eq((uintptr_t)cells.data & 7u, 0u);
	cr_assert(dt_property_read_cells(&cells, 0u, 2u, &value));
	cr_assert_eq(value, UINT64_C(0x123456789abcdef0));
}

Test(dt, accepts_version_2) {
	test_version(2u);
}

Test(dt, accepts_version_3) {
	test_version(3u);
}

Test(dt, accepts_version_15) {
	test_version(15u);
}

Test(dt, accepts_version_16) {
	test_version(16u);
}

Test(dt, accepts_version_17) {
	test_version(17u);
}

Test(dt, unavailable_provider_rejects_navigation_without_outputs) {
	struct dt_property output  = {.data = test_arena, .size = 17u};
	struct dt_node     invalid = DT_NODE_INVALID;

	cr_assert_not(dt_node_valid(dt_root()));
	cr_assert_not(dt_node_valid(dt_node_child(invalid)));
	cr_assert_not(dt_node_valid(dt_node_next(invalid)));
	cr_assert_null(dt_node_name(invalid));
	cr_assert_not(dt_node_property(invalid, "value", &output));
	cr_assert_eq(output.data, test_arena);
	cr_assert_eq(output.size, 17u);
	cr_assert_not(dt_node_enabled(invalid));
	cr_assert_not(dt_node_compatible(invalid, "vendor,device"));
	cr_assert_not(dt_node_valid(dt_node_by_phandle(1u)));
}

Test(dt, traversal_properties_and_standard_predicates) {
	struct dt_property property;
	struct dt_property unchanged = {.data = test_arena, .size = 99u};
	struct dt_node     root;
	struct dt_node     soc;
	struct dt_node     child;
	struct dt_node     off;
	uint64_t           value;

	test_tree(17u);
	cr_assert(dt_init(test_arena));
	root  = dt_root();
	soc   = dt_node_child(root);
	child = dt_node_child(soc);
	off   = dt_node_next(soc);
	cr_assert_str_eq(dt_node_name(root), "");
	cr_assert_str_eq(dt_node_name(soc), "soc");
	cr_assert_str_eq(dt_node_name(child), "child");
	cr_assert_str_eq(dt_node_name(off), "off");
	cr_assert_eq(dt_node_parent(soc).id, root.id);
	cr_assert_eq(dt_node_parent(child).id, soc.id);
	cr_assert_eq(dt_node_parent(off).id, root.id);
	cr_assert_not(dt_node_valid(dt_node_parent(root)));
	cr_assert_not(dt_node_valid(dt_node_next(child)));
	cr_assert_not(dt_node_valid(dt_node_next(off)));
	cr_assert(dt_node_enabled(soc));
	cr_assert_not(dt_node_enabled(off));
	cr_assert(dt_node_compatible(soc, "vendor,soc"));
	cr_assert(dt_node_compatible(soc, "vendor,fallback"));
	cr_assert_not(dt_node_compatible(soc, "vendor,other"));
	cr_assert(dt_node_property(soc, "cells", &property));
	cr_assert(dt_property_read_cells(&property, 0u, 1u, &value));
	cr_assert_eq(value, 0x12345678u);
	cr_assert(dt_property_read_cells(&property, 0u, 2u, &value));
	cr_assert_eq(value, UINT64_C(0x123456789abcdef0));
	value = UINT64_MAX;
	cr_assert_not(dt_property_read_cells(&property, 1u, 2u, &value));
	cr_assert_eq(value, UINT64_MAX);
	cr_assert(dt_node_property(child, "empty", &property));
	cr_assert_eq(property.size, 0u);
	cr_assert_not(dt_node_property(child, "missing", &unchanged));
	cr_assert_eq(unchanged.data, test_arena);
	cr_assert_eq(unchanged.size, 99u);
	cr_assert_eq(dt_node_by_phandle(7u).id, soc.id);
	cr_assert_not(dt_node_valid(dt_node_by_phandle(0u)));
	cr_assert_not(dt_node_valid((struct dt_node){.id = SIZE_MAX}));
	cr_assert_not(dt_node_valid((struct dt_node){.id = 2u}));
	cr_assert_eq(dt_device_count("vendor,soc"), 1u);
	cr_assert_eq(dt_device_at("vendor,soc", 0u).id, soc.id);
	cr_assert_not(dt_node_valid(dt_device_at("vendor,soc", 1u)));
	cr_assert_eq(dt_node_with_string_at("compatible", "vendor,soc", 0u).id, soc.id);
	cr_assert_eq(dt_node_with_string_at("compatible", "vendor,soc", 1u).id, off.id);
	cr_assert_not(dt_node_valid(dt_node_with_string_at("compatible", "missing", 0u)));
}

Test(dt, navigation_accepts_nop_before_root) {
	struct dt_node root;
	struct dt_node child;

	test_reset();
	test_token(4u);
	test_begin_node("");
	test_begin_node("child");
	test_end_node();
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	root  = dt_root();
	child = dt_node_child(root);
	cr_assert_eq(root.id, 4u);
	cr_assert_str_eq(dt_node_name(child), "child");
	cr_assert_eq(dt_node_parent(child).id, root.id);
}

Test(dt, decodes_and_translates_device_registers) {
	static const uint8_t root_address_cells[] = {0u, 0u, 0u, 2u};
	static const uint8_t root_size_cells[]    = {0u, 0u, 0u, 2u};
	static const uint8_t bus_address_cells[]  = {0u, 0u, 0u, 1u};
	static const uint8_t bus_size_cells[]     = {0u, 0u, 0u, 1u};
	static const uint8_t ranges[]             = {0u, 0u, 0x10u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u};
	static const uint8_t compatible[]         = "vendor,device\0";
	static const uint8_t reg[]                = {0u, 0u, 0x12u, 0u, 0u, 0u, 1u, 0u};
	size_t               address_cells_name;
	size_t               size_cells_name;
	size_t               ranges_name;
	size_t               compatible_name;
	size_t               reg_name;
	struct dt_node       device;
	struct dt_reg        decoded;

	test_reset();
	address_cells_name = test_string("#address-cells");
	size_cells_name    = test_string("#size-cells");
	ranges_name        = test_string("ranges");
	compatible_name    = test_string("compatible");
	reg_name           = test_string("reg");
	test_begin_node("");
	test_property(17u, address_cells_name, root_address_cells, sizeof(root_address_cells));
	test_property(17u, size_cells_name, root_size_cells, sizeof(root_size_cells));
	test_begin_node("bus");
	test_property(17u, address_cells_name, bus_address_cells, sizeof(bus_address_cells));
	test_property(17u, size_cells_name, bus_size_cells, sizeof(bus_size_cells));
	test_property(17u, ranges_name, ranges, sizeof(ranges));
	test_begin_node("device@1200");
	test_property(17u, compatible_name, compatible, sizeof(compatible));
	test_property(17u, reg_name, reg, sizeof(reg));
	test_end_node();
	test_end_node();
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	cr_assert_eq(dt_device_count("vendor,device"), 1u);
	device = dt_device_at("vendor,device", 0u);
	cr_assert(dt_node_valid(device));
	cr_assert(dt_node_reg_raw(device, 0u, &decoded));
	cr_assert_eq(decoded.address, 0x1200u);
	cr_assert_eq(decoded.size, 0x100u);
	cr_assert(dt_node_reg(device, 0u, &decoded));
	cr_assert_eq(decoded.address, UINT64_C(0x100000200));
	cr_assert_eq(decoded.size, 0x100u);
}

Test(dt, rejects_offsets_inside_tokens) {
	static const uint8_t fake_node[] = {0u, 0u, 0u, 1u};
	struct dt_property   property;
	size_t               property_name;
	size_t               fake_id;

	test_reset();
	property_name = test_string("value");
	test_begin_node("");
	test_property(17u, property_name, fake_node, sizeof(fake_node));
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	cr_assert(dt_node_property(dt_root(), "value", &property));
	fake_id = (size_t)((const uint8_t*)property.data - (test_arena + TEST_STRUCTURE_OFFSET));
	cr_assert_eq(fake_id & 3u, 0u);
	cr_assert_not(dt_node_valid((struct dt_node){.id = fake_id}));
}

Test(dt, accepts_property_padding_contents_but_rejects_name_padding) {
	static const uint8_t value[] = {1u};
	struct dt_property   property;
	struct dt_node       child;
	size_t               property_name;

	test_reset();
	property_name = test_string("value");
	test_begin_node("");
	test_begin_node("a");
	test_property(17u, property_name, value, sizeof(value));
	test_end_node();
	test_end_node();
	test_token(9u);
	test_header(17u);
	test_arena[TEST_STRUCTURE_OFFSET + 14u] = 1u;
	cr_assert_not(dt_init(test_arena));
	test_arena[TEST_STRUCTURE_OFFSET + 14u] = 0u;
	test_arena[TEST_STRUCTURE_OFFSET + 29u] = 1u;
	cr_assert(dt_init(test_arena));
	child = dt_node_child(dt_root());
	cr_assert(dt_node_property(child, "value", &property));
	cr_assert_eq(property.size, sizeof(value));
	cr_assert_eq(((const uint8_t*)property.data)[0], value[0]);
}

Test(dt, failed_initialization_retries_and_success_is_immutable) {
	test_tree(17u);
	test_arena[0] = 0u;
	cr_assert_not(dt_init(test_arena));
	test_write_u32(test_arena, 0xd00dfeedu);
	cr_assert(dt_init(test_arena));
	cr_assert(dt_init(NULL));
}

Test(dt, validates_hhdm_alignment_and_complete_memory_coverage) {
	test_tree(17u);
	cr_assert_not(dt_init(test_arena + 1u));
	test_ranges[0].type = MEM_RANGE_RESERVED;
	cr_assert_not(dt_init(test_arena));
	test_ranges[0].type   = MEM_RANGE_BOOTLOADER_RECLAIMABLE;
	test_ranges[0].length = 0x800u;
	test_ranges[1]        = (struct mem_range){
		.base   = TEST_PHYSICAL_BASE + 0x801u,
		.length = TEST_ARENA_SIZE - 0x801u,
		.type   = MEM_RANGE_BOOTLOADER_RECLAIMABLE,
	};
	test_range_count = 2u;
	cr_assert_not(dt_init(test_arena));
	test_ranges[1].base--;
	test_ranges[1].length++;
	cr_assert(dt_init(test_arena));
}

Test(dt, rejects_bad_versions_sizes_blocks_and_structure) {
	test_tree(17u);
	test_write_u32(test_arena + 20u, 1u);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + 24u, 18u);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + 24u, 0u);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + 4u, 16u * 1024u * 1024u + 1u);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + 16u, TEST_TOTAL_SIZE - 8u);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + 12u, TEST_STRUCTURE_OFFSET);
	cr_assert_not(dt_init(test_arena));
	test_header(17u);
	test_write_u32(test_arena + TEST_STRUCTURE_OFFSET, 8u);
	cr_assert_not(dt_init(test_arena));
}

Test(dt, resolves_legacy_phandles) {
	static const uint8_t phandle[] = {0u, 0u, 0u, 9u};
	struct dt_node       node;
	size_t               phandle_name;

	test_reset();
	phandle_name = test_string("linux,phandle");
	test_begin_node("");
	test_begin_node("legacy");
	test_property(17u, phandle_name, phandle, sizeof(phandle));
	test_end_node();
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	node = dt_node_by_phandle(9u);
	cr_assert(dt_node_valid(node));
	cr_assert_str_eq(dt_node_name(node), "legacy");
}

Test(dt, traverses_without_a_fixed_depth_limit) {
	struct dt_node node;

	test_reset();
	test_begin_node("");
	for (size_t depth = 0u; depth < 40u; depth++) test_begin_node("n");
	for (size_t depth = 0u; depth < 40u; depth++) test_end_node();
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	node = dt_root();
	for (size_t depth = 0u; depth < 40u; depth++) {
		node = dt_node_child(node);
		cr_assert(dt_node_valid(node));
	}
	cr_assert_not(dt_node_valid(dt_node_child(node)));
}

Test(dt, rejects_ambiguous_phandles_and_malformed_string_lists) {
	static const uint8_t compatible[] = {'o', 'k'};
	static const uint8_t phandle[]    = {0u, 0u, 0u, 5u};
	struct dt_property   property;
	size_t               compatible_name;
	size_t               phandle_name;

	test_reset();
	compatible_name = test_string("compatible");
	phandle_name    = test_string("phandle");
	test_begin_node("");
	for (size_t index = 0u; index < 2u; index++) {
		test_begin_node("node");
		test_property(17u, compatible_name, compatible, sizeof(compatible));
		test_property(17u, phandle_name, phandle, sizeof(phandle));
		test_end_node();
	}
	test_end_node();
	test_token(9u);
	test_header(17u);
	cr_assert(dt_init(test_arena));
	cr_assert_not(dt_node_valid(dt_node_by_phandle(5u)));
	cr_assert(dt_node_property(dt_node_child(dt_root()), "compatible", &property));
	cr_assert_not(dt_property_string_list_contains(&property, "ok"));
}
