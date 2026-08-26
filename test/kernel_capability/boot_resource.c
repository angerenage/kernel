#include "../../kernel/src/capability/boot_resource.h"

#include <base/boot_data.h>
#include <base/framebuffer.h>
#include <base/kernel_resource.h>

#include "test_support.h"

Test(kernel_capability_boot_resource, rsdp_and_dtb_caps_report_and_read_immutable_data) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  rsdp[]       = {0x52u, 0x53u, 0x44u, 0x50u};
	static const uint8_t                  dtb[]        = {0xd0u, 0x0du, 0xfeu, 0xedu, 0x11u};
	const struct boot_data_info_request   info_request = {.header = {.op = BOOT_DATA_OP_INFO}};
	const struct boot_data_read_request read_request = {.header = {.op = BOOT_DATA_OP_READ}, .offset = 1u, .size = 3u};
	struct boot_data_info_response      info;
	struct capability*                  root;
	uint8_t                             bytes[3];
	cap_id_t                            rsdp_cap;
	cap_id_t                            dtb_cap;
	cap_id_t                            call_only_cap;
	syscall_result_t                    result;

	kernel_capability_test_begin(&ctx, "kernel-cap/boot-data");
	kernel_boot_mock_set_rsdp(rsdp, sizeof(rsdp));
	kernel_boot_mock_set_dtb(dtb, sizeof(dtb));
	kernel_capability_boot_resources_init();
	rsdp_cap = kernel_capability_boot_data_grant(KERNEL_RESOURCE_TYPE_RSDP, process_pid(ctx.process));
	dtb_cap  = kernel_capability_boot_data_grant(KERNEL_RESOURCE_TYPE_DTB, process_pid(ctx.process));
	cr_assert_neq(rsdp_cap, CAP_ID_INVALID);
	cr_assert_neq(dtb_cap, CAP_ID_INVALID);

	result = kernel_capability_test_call(rsdp_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.type, KERNEL_RESOURCE_TYPE_RSDP);
	cr_assert_eq(info.size, sizeof(rsdp));
	result = kernel_capability_test_call(dtb_cap, &read_request, sizeof(read_request), bytes, sizeof(bytes));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_arr_eq(bytes, dtb + 1u, sizeof(bytes));

	struct boot_data_read_request boundary_request = {
		.header = {.op = BOOT_DATA_OP_READ}, .offset = sizeof(dtb), .size = 0u};
	result = kernel_capability_test_call(dtb_cap, &boundary_request, sizeof(boundary_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	boundary_request.size = 1u;
	result = kernel_capability_test_call(dtb_cap, &boundary_request, sizeof(boundary_request), bytes, sizeof(bytes));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	result = kernel_capability_test_call(dtb_cap, &info_request, 1u, &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	root = cap_acquire(dtb_cap);
	cr_assert_not_null(root);
	call_only_cap = cap_create(root->cap_object_id, process_pid(ctx.process), CAP_CALL, root);
	cap_release(root);
	cr_assert_neq(call_only_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(call_only_cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_boot_resource, framebuffer_reports_format_and_maps_writable) {
	struct kernel_capability_test_context ctx;
	const struct framebuffer_info_request info_request = {.header = {.op = FRAMEBUFFER_OP_INFO}};
	const struct framebuffer_map_request  map_request  = {.header = {.op = FRAMEBUFFER_OP_MAP}};
	struct framebuffer_info_response      info;
	struct framebuffer_map_response       mapping;
	struct kernel_boot_framebuffer        framebuffer;
	struct capability*                    root;
	uintptr_t                             physical;
	cap_id_t                              cap;
	cap_id_t                              read_only_cap;
	size_t                                caps_before;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/framebuffer");
	cr_assert(pmm_alloc_pages(1u, &physical));
	framebuffer = (struct kernel_boot_framebuffer){
		.address          = (void*)(physical + 31u),
		.width            = 8u,
		.height           = 4u,
		.pitch            = 32u,
		.bpp              = 32u,
		.memory_model     = 1u,
		.red_mask_size    = 8u,
		.red_mask_shift   = 16u,
		.green_mask_size  = 8u,
		.green_mask_shift = 8u,
		.blue_mask_size   = 8u,
	};
	kernel_boot_mock_set_framebuffer(&framebuffer);
	kernel_capability_boot_resources_init();
	cap = kernel_capability_framebuffer_grant(process_pid(ctx.process));
	cr_assert_neq(cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(cap, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.size, 128u);
	cr_assert_eq(info.red_mask_shift, 16u);
	caps_before = capability_count();
	result      = kernel_capability_test_call(cap, &map_request, sizeof(map_request), &mapping, sizeof(mapping) - 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before);
	result = kernel_capability_test_call(cap, &map_request, sizeof(map_request), &mapping, sizeof(mapping));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(mapping.mapping.prot, VMM_PROT_READ | VMM_PROT_WRITE);
	cr_assert_eq(mapping.data_offset, 31u);
	cr_assert_neq(mapping.mapping_cap, CAP_ID_INVALID);

	root = cap_acquire(cap);
	cr_assert_not_null(root);
	read_only_cap = cap_create(root->cap_object_id, process_pid(ctx.process), CAP_CALL | CAP_READ, root);
	cap_release(root);
	cr_assert_neq(read_only_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(read_only_cap, &map_request, sizeof(map_request), &mapping, sizeof(mapping));
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	kernel_capability_test_end(&ctx);
}
