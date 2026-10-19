#include <criterion/criterion.h>
#include <kernel/crash_qr.h>
#include <string.h>

static bool qr_module(const struct kernel_crash_qr* qr, size_t x, size_t y) {
	size_t index = y * qr->size + x;
	return ((qr->modules[(index >> 3u) + 1u] >> (index & 7u)) & 1u) != 0u;
}

static struct boot_framebuffer rgb32_framebuffer(void* pixels, size_t width, size_t height) {
	return (struct boot_framebuffer){
		.address          = pixels,
		.width            = width,
		.height           = height,
		.pitch            = width * 4u,
		.bpp              = 32u,
		.memory_model     = 1u,
		.red_mask_size    = 8u,
		.red_mask_shift   = 16u,
		.green_mask_size  = 8u,
		.green_mask_shift = 8u,
		.blue_mask_size   = 8u,
		.blue_mask_shift  = 0u,
	};
}

Test(crash_qr, encodes_a_bounded_byte_payload) {
	struct kernel_crash_qr qr;

	cr_assert(kernel_crash_qr_encode(&qr, "x", 1u));
	cr_assert_eq(qr.size, 21u);
	cr_assert(qr_module(&qr, 0u, 0u));
	cr_assert(qr_module(&qr, 6u, 0u));
	cr_assert_not(qr_module(&qr, 1u, 1u));
	cr_assert_not(kernel_crash_qr_encode(&qr, "", 0u));
}

Test(crash_qr, draws_a_centered_high_contrast_symbol_on_blue) {
	static uint8_t          pixels[96u * 96u * 4u];
	struct kernel_crash_qr  qr;
	struct boot_framebuffer framebuffer  = rgb32_framebuffer(pixels, 96u, 96u);
	const size_t            panel_left   = 19u;
	const size_t            first_module = panel_left + 8u;

	memset(pixels, 0xa5, sizeof(pixels));
	cr_assert(kernel_crash_qr_encode(&qr, "x", 1u));
	cr_assert(kernel_crash_qr_draw(&qr, &framebuffer));
	cr_assert_eq(pixels[0], 0xd7u);
	cr_assert_eq(pixels[1], 0x78u);
	cr_assert_eq(pixels[2], 0x00u);
	cr_assert_eq(pixels[(panel_left * 96u + panel_left) * 4u], 0xffu);
	cr_assert_eq(pixels[(first_module * 96u + first_module) * 4u], 0x00u);
}

Test(crash_qr, rejects_an_unsafe_framebuffer_without_writing) {
	uint8_t                 pixels[64u];
	struct kernel_crash_qr  qr;
	struct boot_framebuffer framebuffer = rgb32_framebuffer(pixels, 4u, 4u);

	memset(pixels, 0xa5, sizeof(pixels));
	cr_assert(kernel_crash_qr_encode(&qr, "x", 1u));
	framebuffer.pitch = 1u;
	cr_assert_not(kernel_crash_qr_draw(&qr, &framebuffer));
	for (size_t index = 0u; index < sizeof(pixels); index++) cr_assert_eq(pixels[index], 0xa5u);
}
