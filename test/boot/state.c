#include <criterion/criterion.h>

#include "../../kernel/src/boot/limine_requests.h"
#include "test_support.h"

Test(boot_state, failed_initialization_does_not_publish_partial_boot_state) {
	struct kernel_boot_address_space address_space    = {0};
	size_t                           memory_map_count = 123u;

	boot_test_configure_valid_base();
	boot_test_configure_module_count(65u);

	cr_assert_not(kernel_boot_init());
	cr_assert_not(kernel_boot_protocol_supported());
	cr_assert_null(kernel_boot_memmap(&memory_map_count));
	cr_assert_not(kernel_boot_address_space_get(&address_space),
	              "failed initialization exposed a partial address-space snapshot");
}

Test(boot_state, successful_initialization_publishes_a_coherent_snapshot) {
	struct kernel_boot_address_space address_space = {0};
	const struct mem_range*          memory_map;
	size_t                           memory_map_count = 0u;

	boot_test_configure_valid_base();
	cr_assert(kernel_boot_init());
	cr_assert(kernel_boot_protocol_supported());

	memory_map = kernel_boot_memmap(&memory_map_count);
	cr_assert_not_null(memory_map);
	cr_assert_eq(memory_map_count, 1u);
	cr_assert_eq(memory_map[0].base, 0x100000u);
	cr_assert_eq(memory_map[0].length, 0x400000u);
	cr_assert_eq(memory_map[0].type, MEM_RANGE_USABLE);

	cr_assert(kernel_boot_address_space_get(&address_space));
	cr_assert_eq(address_space.direct_map_offset, 0xffff800000000000ull);
	cr_assert_eq(address_space.physical_base, 0x200000u);
	cr_assert_eq(address_space.virtual_base, 0xffffffff80000000ull);
}

Test(boot_state, successful_initialization_is_idempotent) {
	struct kernel_boot_address_space before;
	struct kernel_boot_address_space after;

	boot_test_configure_valid_base();
	cr_assert(kernel_boot_init());
	cr_assert(kernel_boot_address_space_get(&before));

	hhdm_req.response      = NULL;
	exec_addr_req.response = NULL;
	memmap_req.response    = NULL;

	cr_assert(kernel_boot_init());
	cr_assert(kernel_boot_address_space_get(&after));
	cr_assert_eq(after.direct_map_offset, before.direct_map_offset);
	cr_assert_eq(after.physical_base, before.physical_base);
	cr_assert_eq(after.virtual_base, before.virtual_base);
}

Test(boot_state, framebuffer_format_is_validated_and_published) {
	static uint8_t                            framebuffer_memory[4096];
	static struct limine_framebuffer          framebuffer;
	static struct limine_framebuffer*         framebuffers[1];
	static struct limine_framebuffer_response framebuffer_response;
	struct kernel_boot_framebuffer            captured;

	boot_test_configure_valid_base();
	framebuffer = (struct limine_framebuffer){
		.address          = framebuffer_memory,
		.width            = 32u,
		.height           = 16u,
		.pitch            = 128u,
		.bpp              = 32u,
		.memory_model     = LIMINE_FRAMEBUFFER_RGB,
		.red_mask_size    = 8u,
		.red_mask_shift   = 16u,
		.green_mask_size  = 8u,
		.green_mask_shift = 8u,
		.blue_mask_size   = 8u,
		.blue_mask_shift  = 0u,
	};
	framebuffers[0]      = &framebuffer;
	framebuffer_response = (struct limine_framebuffer_response){.framebuffer_count = 1u, .framebuffers = framebuffers};
	fb_req.response      = &framebuffer_response;

	cr_assert(kernel_boot_init());
	cr_assert(kernel_boot_framebuffer_get(&captured));
	cr_assert_eq(captured.address, framebuffer_memory);
	cr_assert_eq(captured.pitch, 128u);
	cr_assert_eq(captured.memory_model, LIMINE_FRAMEBUFFER_RGB);
	cr_assert_eq(captured.red_mask_shift, 16u);
}
