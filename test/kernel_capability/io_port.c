#include "../../kernel/src/capability/io_port.h"

#include <base/io_port.h>
#include <core/capability.h>
#include <core/io_port.h>

#include "../mocks/base/io_port_mock.h"
#include "../mocks/hal/io_port_mock.h"
#include "test_support.h"

static cap_id_t derive(struct kernel_capability_test_context* ctx, cap_id_t parent, uint32_t offset, uint32_t count,
                       cap_rights_t rights) {
	const struct io_port_derive_request request = {
		.header = {.op = IO_PORT_OP_DERIVE},
		.offset = offset,
		.count  = count,
		.rights = rights,
	};
	struct io_port_derive_response response = {0};
	syscall_result_t               result =
		kernel_capability_test_call(parent, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(response.io_port_cap, CAP_ID_INVALID);
	(void)ctx;
	return response.io_port_cap;
}

static cap_id_t init_root(struct kernel_capability_test_context* ctx) {
	cr_assert(kernel_capability_io_ports_init());
	cap_id_t cap = kernel_capability_io_ports_grant(process_pid(ctx->process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	return cap;
}

Test(kernel_capability_io_port, root_and_derived_ranges_report_geometry_and_reduced_rights) {
	struct kernel_capability_test_context ctx;
	const struct io_port_simple_request   info_request = {.header = {.op = IO_PORT_OP_INFO}};
	struct io_port_info                   info;
	struct capability*                    capability;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/io-port-ranges");
	cap_id_t root = init_root(&ctx);
	result        = kernel_capability_test_call(root, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.base, 0u);
	cr_assert_eq(info.count, IO_PORT_COUNT);

	cap_id_t child = derive(&ctx, root, 0x3f8u, 8u, CAP_CALL | CAP_READ | CAP_MAP);
	result         = kernel_capability_test_call(child, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.base, 0x3f8u);
	cr_assert_eq(info.count, 8u);
	capability = cap_acquire(child);
	cr_assert_not_null(capability);
	cr_assert_eq(cap_rights(capability), CAP_CALL | CAP_READ | CAP_MAP);
	cap_release(capability);

	const struct io_port_derive_request outside = {
		.header = {.op = IO_PORT_OP_DERIVE}, .offset = IO_PORT_COUNT - 1u, .count = 2u, .rights = CAP_CALL};
	struct io_port_derive_response response;
	result = kernel_capability_test_call(root, &outside, sizeof(outside), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	const struct io_port_derive_request escalated = {
		.header = {.op = IO_PORT_OP_DERIVE}, .offset = 0u, .count = 1u, .rights = UINT64_MAX};
	result = kernel_capability_test_call(root, &escalated, sizeof(escalated), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_io_port, direct_access_validates_width_boundaries_and_rights_before_touching_hardware) {
	struct kernel_capability_test_context ctx;
	struct io_port_read_request           read_request = {
		.header = {.op = IO_PORT_OP_READ}, .offset = 4u, .width = IO_PORT_WIDTH_32};
	struct io_port_read_response read_response;
	struct io_port_write_request write_request = {
		.header = {.op = IO_PORT_OP_WRITE}, .offset = 1u, .width = IO_PORT_WIDTH_16, .value = 0x12345678u};
	syscall_result_t result;

	kernel_capability_test_begin(&ctx, "kernel-cap/io-port-direct");
	io_port_instruction_mock_reset();
	cap_id_t root  = init_root(&ctx);
	cap_id_t child = derive(&ctx, root, 0x3f8u, 8u, CAP_CALL | CAP_READ | CAP_WRITE);
	io_port_instruction_mock_set_read_value(0x89abcdefu);
	result =
		kernel_capability_test_call(child, &read_request, sizeof(read_request), &read_response, sizeof(read_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(read_response.value, 0x89abcdefu);
	cr_assert_eq(io_port_instruction_mock_read_count(), 1u);
	cr_assert_eq(io_port_instruction_mock_last_port(), 0x3fcu);
	cr_assert_eq(io_port_instruction_mock_last_width(), IO_PORT_WIDTH_32);

	read_request.offset = 5u;
	result =
		kernel_capability_test_call(child, &read_request, sizeof(read_request), &read_response, sizeof(read_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(io_port_instruction_mock_read_count(), 1u);
	read_request.offset = IO_PORT_COUNT - 1u;
	read_request.width  = IO_PORT_WIDTH_16;
	result =
		kernel_capability_test_call(root, &read_request, sizeof(read_request), &read_response, sizeof(read_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(io_port_instruction_mock_read_count(), 1u);
	read_request.offset = 0u;
	read_request.width  = 3u;
	result =
		kernel_capability_test_call(child, &read_request, sizeof(read_request), &read_response, sizeof(read_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(io_port_instruction_mock_read_count(), 1u);

	result = kernel_capability_test_call(child, &write_request, sizeof(write_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(io_port_instruction_mock_write_count(), 1u);
	cr_assert_eq(io_port_instruction_mock_last_port(), 0x3f9u);
	cr_assert_eq(io_port_instruction_mock_last_width(), IO_PORT_WIDTH_16);
	cr_assert_eq(io_port_instruction_mock_last_write_value(), 0x5678u);

	cap_id_t read_only   = derive(&ctx, root, 0x80u, 1u, CAP_CALL | CAP_READ);
	write_request.offset = 0u;
	write_request.width  = IO_PORT_WIDTH_8;
	result               = kernel_capability_test_call(read_only, &write_request, sizeof(write_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(io_port_instruction_mock_write_count(), 1u);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_io_port, mappings_are_per_grant_and_map_right_revocation_is_synchronous) {
	struct kernel_capability_test_context ctx;
	const struct io_port_simple_request   map_request   = {.header = {.op = IO_PORT_OP_MAP}};
	const struct io_port_simple_request   unmap_request = {.header = {.op = IO_PORT_OP_UNMAP}};
	struct capability*                    capability;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/io-port-map");
	hal_io_port_mock_reset();
	cap_id_t root   = init_root(&ctx);
	cap_id_t first  = derive(&ctx, root, 0x100u, 0x100u, CAP_CALL | CAP_MAP);
	cap_id_t second = derive(&ctx, root, 0x180u, 0x100u, CAP_CALL | CAP_MAP);
	result          = kernel_capability_test_call(first, &map_request, sizeof(map_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	result = kernel_capability_test_call(second, &map_request, sizeof(map_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(io_port_process_mapping_count(ctx.process), 2u);

	result = kernel_capability_test_call(first, &unmap_request, sizeof(unmap_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(io_port_process_mapping_count(ctx.process), 1u);
	result = kernel_capability_test_call(first, &unmap_request, sizeof(unmap_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	capability = cap_acquire(second);
	cr_assert_not_null(capability);
	cr_assert(cap_remove_rights(capability, CAP_MAP));
	cap_release(capability);
	cr_assert_eq(io_port_process_mapping_count(ctx.process), 0u);
	cr_assert_gt(hal_io_port_mock_invalidation_count(), 0u);
	result = kernel_capability_test_call(second, &map_request, sizeof(map_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_io_port, destroying_one_mapped_grant_preserves_an_overlapping_grant) {
	struct kernel_capability_test_context ctx;
	const struct io_port_simple_request   map_request = {.header = {.op = IO_PORT_OP_MAP}};

	kernel_capability_test_begin(&ctx, "kernel-cap/io-port-revoke");
	cap_id_t root   = init_root(&ctx);
	cap_id_t first  = derive(&ctx, root, 0x200u, 0x100u, CAP_CALL | CAP_MAP);
	cap_id_t second = derive(&ctx, root, 0x280u, 0x100u, CAP_CALL | CAP_MAP);
	cr_assert_eq(kernel_capability_test_call(first, &map_request, sizeof(map_request), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(kernel_capability_test_call(second, &map_request, sizeof(map_request), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	cr_assert(cap_destroy_by_id(first));
	cr_assert_eq(io_port_process_mapping_count(ctx.process), 1u);
	io_port_process_load(ctx.process);
	const struct hal_io_port_bitmap* bitmap = hal_io_port_mock_loaded_bitmap();
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x280u));
	cr_assert_not(hal_io_port_bitmap_port_allowed(bitmap, 0x200u));
	kernel_capability_test_end(&ctx);
}
