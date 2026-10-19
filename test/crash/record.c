#include <criterion/criterion.h>
#include <kernel/crash.h>
#include <stdint.h>
#include <string.h>

#include "kernel_config.h"

static void write_u32(uint8_t* bytes, uint32_t value) {
	memcpy(bytes, &value, sizeof(value));
}

static uint64_t read_compact_u64(const uint8_t* bytes, size_t* cursor) {
	uint8_t  tag    = bytes[(*cursor)++];
	size_t   length = tag & 0x0fu;
	uint64_t value  = (tag & 0x10u) != 0u ? UINT64_MAX : 0u;

	for (size_t i = 0u; i < length; i++) {
		value &= ~((uint64_t)0xffu << (8u * i));
		value |= (uint64_t)bytes[(*cursor)++] << (8u * i);
	}
	return value;
}

Test(crash_record, reset_publishes_schema_source_and_architecture) {
	struct kernel_crash_record record;

	memset(&record, 0xa5, sizeof(record));
	kernel_crash_record_reset(&record, KERNEL_CRASH_ARCH_RISCV64, KERNEL_CRASH_CAUSE_PANIC);
	cr_assert_eq(record.schema_version, KERNEL_CRASH_SCHEMA_VERSION);
	cr_assert_eq(record.arch, KERNEL_CRASH_ARCH_RISCV64);
	cr_assert_eq(record.cause, KERNEL_CRASH_CAUSE_PANIC);
	cr_assert_str_eq(record.git_commit, KERNEL_GIT_COMMIT);
	cr_assert_eq(record.register_count, 0u);
}

Test(crash_record, parses_a_gnu_sha1_build_id_note) {
	uint8_t note[36] = {0};
	uint8_t parsed[KERNEL_CRASH_BUILD_ID_SIZE];

	write_u32(note, 4u);
	write_u32(note + 4u, KERNEL_CRASH_BUILD_ID_SIZE);
	write_u32(note + 8u, 3u);
	memcpy(note + 12u, "GNU\0", 4u);
	for (uint8_t i = 0u; i < KERNEL_CRASH_BUILD_ID_SIZE; i++) note[16u + i] = i;
	cr_assert(kernel_crash_parse_build_id(note, sizeof(note), parsed));
	for (uint8_t i = 0u; i < KERNEL_CRASH_BUILD_ID_SIZE; i++) cr_assert_eq(parsed[i], i);
}

Test(crash_record, rejects_truncated_and_non_gnu_build_id_notes) {
	uint8_t note[36] = {0};
	uint8_t parsed[KERNEL_CRASH_BUILD_ID_SIZE];

	write_u32(note, 4u);
	write_u32(note + 4u, KERNEL_CRASH_BUILD_ID_SIZE);
	write_u32(note + 8u, 3u);
	memcpy(note + 12u, "BAD\0", 4u);
	cr_assert_not(kernel_crash_parse_build_id(note, sizeof(note), parsed));
	memcpy(note + 12u, "GNU\0", 4u);
	cr_assert_not(kernel_crash_parse_build_id(note, sizeof(note) - 1u, parsed));
}

Test(crash_record, walks_frame_records_at_the_frame_pointer) {
	uintptr_t                  stack[6] = {0};
	struct kernel_crash_record record   = {.instruction_pointer = 0x1000u, .frame_pointer = (uintptr_t)&stack[0]};

	stack[0] = (uintptr_t)&stack[2];
	stack[1] = 0x2000u;
	stack[2] = 0u;
	stack[3] = 0x3000u;
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[6], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert_eq(record.trace_count, 3u);
	cr_assert_eq(record.trace[0], 0x1000u);
	cr_assert_eq(record.trace[1], 0x2000u);
	cr_assert_eq(record.trace[2], 0x3000u);
	cr_assert_eq(record.trace_flags, 0u);
}

Test(crash_record, walks_frame_records_immediately_below_the_frame_pointer) {
	uintptr_t                  stack[8] = {0};
	struct kernel_crash_record record   = {
		.instruction_pointer = 0x1000u,
		.frame_pointer       = (uintptr_t)&stack[2],
	};

	stack[0] = (uintptr_t)&stack[6];
	stack[1] = 0x2000u;
	stack[4] = 0u;
	stack[5] = 0x3000u;
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[8], KERNEL_CRASH_FRAME_RECORD_BEFORE_FP);
	cr_assert_eq(record.trace_count, 3u);
	cr_assert_eq(record.trace[1], 0x2000u);
	cr_assert_eq(record.trace[2], 0x3000u);
	cr_assert_eq(record.trace_flags, 0u);
}

Test(crash_record, rejects_misaligned_reversed_and_out_of_bounds_frames) {
	uintptr_t                  stack[4] = {0};
	struct kernel_crash_record record   = {.instruction_pointer = 0x1000u};

	record.frame_pointer = (uintptr_t)&stack[0] + 1u;
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[4], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_INVALID);

	record.frame_pointer = (uintptr_t)&stack[2];
	stack[2]             = (uintptr_t)&stack[1];
	stack[3]             = 0x2000u;
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[4], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_INVALID);

	record.frame_pointer = (uintptr_t)&stack[4];
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[4], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_INVALID);
}

Test(crash_record, rejects_cyclic_and_overflowing_frames) {
	uintptr_t                  stack[2] = {0};
	struct kernel_crash_record record   = {.instruction_pointer = 0x1000u, .frame_pointer = (uintptr_t)&stack[0]};

	stack[0] = (uintptr_t)&stack[0];
	stack[1] = 0x2000u;
	kernel_crash_walk_stack(&record, (uintptr_t)&stack[0], (uintptr_t)&stack[2], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_INVALID);

	record.frame_pointer = UINTPTR_MAX & ~(sizeof(uintptr_t) - 1u);
	kernel_crash_walk_stack(&record, record.frame_pointer, UINTPTR_MAX, KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_INVALID);
}

Test(crash_record, marks_an_unavailable_stack_and_a_full_trace) {
	uintptr_t                  stack[2u * KERNEL_CRASH_MAX_TRACE] = {0};
	struct kernel_crash_record record = {.instruction_pointer = 0x1000u, .frame_pointer = 0u};

	kernel_crash_walk_stack(&record, 0u, 0u, KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_UNAVAILABLE);

	record.frame_pointer = (uintptr_t)&stack[0];
	for (size_t i = 0u; i < KERNEL_CRASH_MAX_TRACE; i++) {
		stack[2u * i]      = i + 1u < KERNEL_CRASH_MAX_TRACE ? (uintptr_t)&stack[2u * (i + 1u)] : 0u;
		stack[2u * i + 1u] = 0x2000u + i;
	}
	kernel_crash_walk_stack(
		&record, (uintptr_t)&stack[0], (uintptr_t)&stack[2u * KERNEL_CRASH_MAX_TRACE], KERNEL_CRASH_FRAME_RECORD_AT_FP);
	cr_assert(record.trace_flags & KERNEL_CRASH_TRACE_TRUNCATED);
	cr_assert_eq(record.trace_count, KERNEL_CRASH_MAX_TRACE);
}

Test(crash_record, names_native_register_mappings) {
	cr_assert_str_eq(kernel_crash_register_name(KERNEL_CRASH_ARCH_X86_64, 0u), "rax");
	cr_assert_str_eq(kernel_crash_register_name(KERNEL_CRASH_ARCH_AARCH64, 30u), "x30");
	cr_assert_str_eq(kernel_crash_register_name(KERNEL_CRASH_ARCH_RISCV64, 8u), "s1");
	cr_assert_str_eq(kernel_crash_register_name(KERNEL_CRASH_ARCH_LOONGARCH64, 22u), "fp");
	cr_assert_str_eq(kernel_crash_register_name(KERNEL_CRASH_ARCH_X86_64, KERNEL_CRASH_MAX_REGISTERS), "unknown");
}

Test(crash_record, renders_readable_key_value_output) {
	struct kernel_crash_record record;
	char                       output[KERNEL_CRASH_RENDER_SIZE];

	kernel_crash_record_reset(&record, KERNEL_CRASH_ARCH_AARCH64, KERNEL_CRASH_CAUSE_PANIC);
	record.build_id_valid      = true;
	record.build_id[0]         = 0xab;
	record.cpu_index           = 2u;
	record.instruction_pointer = 0x1234u;
	record.register_count      = 1u;
	record.registers[0]        = 0x55u;
	record.trace_count         = 1u;
	record.trace[0]            = 0x1234u;
	record.panic_reason        = KERNEL_PANIC_EXPLICIT;
	record.panic_detail        = 0x77u;
	cr_assert(kernel_crash_render(&record, output, sizeof(output)));
	cr_assert_not_null(strstr(output, "--- KERNEL CRASH FRAME v1 ---\n"));
	cr_assert_not_null(strstr(output, "build.id=ab00000000000000000000000000000000000000\n"));
	cr_assert_not_null(strstr(output, "system.arch=aarch64\n"));
	cr_assert_not_null(strstr(output, "crash.cause=panic\n"));
	cr_assert_not_null(strstr(output, "register.x0=0x0000000000000055\n"));
	cr_assert_not_null(strstr(output, "trace.0=0x0000000000001234\n"));
	cr_assert_not_null(strstr(output, "--- END KERNEL CRASH FRAME ---\n"));
	cr_assert_not(kernel_crash_render(&record, output, 8u));
}

Test(crash_record, ownership_is_first_writer_wins) {
	uint32_t owner = 0u;

	cr_assert(kernel_crash_try_claim(&owner, 3u));
	cr_assert_eq(owner, 3u);
	cr_assert_not(kernel_crash_try_claim(&owner, 4u));
	cr_assert_eq(owner, 3u);
	cr_assert_not(kernel_crash_try_claim(&owner, 0u));
}

Test(crash_record, encodes_the_bounded_wire_format) {
	struct kernel_crash_record record;
	uint8_t                    wire[KERNEL_CRASH_WIRE_MAX_SIZE];
	size_t                     wire_size;
	size_t                     cursor;

	kernel_crash_record_reset(&record, KERNEL_CRASH_ARCH_X86_64, KERNEL_CRASH_CAUSE_EXCEPTION);
	strlcpy(record.git_commit, "00112233445566778899aabbccddeeff00112233", sizeof(record.git_commit));
	record.build_id_valid      = true;
	record.build_id[0]         = 0xabu;
	record.cpu_count           = 4u;
	record.instruction_pointer = 0x123456789abcdef0ull;
	record.register_count      = 1u;
	record.registers[0]        = 0x55u;
	record.trace_count         = 1u;
	record.trace[0]            = 0x66u;
	wire_size                  = kernel_crash_encode_wire(&record, wire, sizeof(wire));

	cr_assert_gt(wire_size, KERNEL_CRASH_WIRE_HEADER_SIZE);
	cr_assert_lt(wire_size, 100u);
	cr_assert_eq(memcmp(wire, "KCF1", 4u), 0);
	cr_assert_eq(wire[4], KERNEL_CRASH_SCHEMA_VERSION);
	cr_assert_eq(wire[7], KERNEL_CRASH_WIRE_GIT_VALID | KERNEL_CRASH_WIRE_BUILD_ID_VALID);
	cr_assert_eq(wire[8], 1u);
	cr_assert_eq(wire[9], 1u);
	cr_assert_eq(wire[10], 0x00u);
	cr_assert_eq(wire[11], 0x11u);
	cr_assert_eq(wire[30], 0xabu);
	cursor = KERNEL_CRASH_WIRE_HEADER_SIZE;
	for (size_t i = 0u; i < 4u; i++) (void)read_compact_u64(wire, &cursor);
	cr_assert_eq(read_compact_u64(wire, &cursor), 4u);
	for (size_t i = 0u; i < 5u; i++) (void)read_compact_u64(wire, &cursor);
	cr_assert_eq(read_compact_u64(wire, &cursor), 0x123456789abcdef0ull);
	for (size_t i = 0u; i < 5u; i++) (void)read_compact_u64(wire, &cursor);
	cr_assert_eq(read_compact_u64(wire, &cursor), 0x55u);
	cr_assert_eq(read_compact_u64(wire, &cursor), 0x66u);
	cr_assert_eq(cursor, wire_size);
	cr_assert_eq(kernel_crash_encode_wire(&record, wire, wire_size - 1u), 0u);
}

Test(crash_record, formats_an_unpadded_base64url_fragment) {
	const uint8_t wire[] = {0xfbu, 0xffu, 0xefu};
	char          url[64];

	cr_assert(kernel_crash_format_url(wire, sizeof(wire), "https://crash.example", url, sizeof(url)));
	cr_assert_str_eq(url, "https://crash.example/crash#v1.-__v");
	cr_assert(kernel_crash_format_url(wire, 2u, "http://localhost:3000/", url, sizeof(url)));
	cr_assert_str_eq(url, "http://localhost:3000/crash#v1.-_8");
	cr_assert_not(kernel_crash_format_url(wire, sizeof(wire), "", url, sizeof(url)));
	cr_assert_not(kernel_crash_format_url(wire, sizeof(wire), "https://crash.example", url, 8u));
}
