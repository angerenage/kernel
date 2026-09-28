#include "../../kernel/src/capability/boot_resource.h"

#include <base/framebuffer.h>
#include <test_memory.h>

#include "test_support.h"

Test(kernel_capability_boot_resource, framebuffer_reports_format_and_maps_writable) {
	struct kernel_capability_test_context ctx;
	const struct framebuffer_info_request info_request = {.header = {.op = FRAMEBUFFER_OP_INFO}};
	const struct framebuffer_map_request  map_request  = {.header = {.op = FRAMEBUFFER_OP_MAP}};
	struct framebuffer_info_response      info;
	struct framebuffer_map_response       mapping;
	struct kernel_boot_framebuffer        framebuffer;
	struct capability*                    root;
	struct pmm_extent                     allocation;
	uintptr_t                             physical;
	cap_id_t                              cap;
	cap_id_t                              read_only_cap;
	size_t                                caps_before;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/framebuffer");
	cr_assert(
		pmm_alloc(&(const struct pmm_alloc_request){.size = TEST_MAPPING_GRANULE, .alignment = TEST_MAPPING_GRANULE},
	              &allocation));
	physical = allocation.address;
	cr_assert(pmm_free(allocation));
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
	cr_assert_eq(mapping.mapping_size, TEST_MAPPING_GRANULE);
	cr_assert_eq(mapping.data_offset, 31u);
	cr_assert_neq(mapping.mapping_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(cap, &map_request, sizeof(map_request), &mapping, sizeof(mapping));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK, "a second mapping must share the resource backing");
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
