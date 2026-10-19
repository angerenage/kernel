#include <kernel/crash_qr.h>
#include <limits.h>
#include <qrcodegen.h>
#include <string.h>

enum {
	CRASH_QR_QUIET_ZONE      = 4u,
	CRASH_FRAMEBUFFER_RGB    = 1u,
	CRASH_QR_MIN_PIXEL_SCALE = 2u,
	CRASH_QR_BLUE_RED        = 0x00u,
	CRASH_QR_BLUE_GREEN      = 0x78u,
	CRASH_QR_BLUE_BLUE       = 0xd7u,
};

struct crash_framebuffer_layout {
	volatile uint8_t* pixels;
	size_t            width;
	size_t            height;
	size_t            pitch;
	size_t            bytes_per_pixel;
	uint32_t          blue;
	uint32_t          white;
	uint32_t          black;
};

static bool crash_channel_valid(uint8_t size, uint8_t shift, uint16_t bpp) {
	return size != 0u && size <= 16u && shift < bpp && size <= bpp - shift;
}

static uint32_t crash_channel_mask(uint8_t size, uint8_t shift) {
	return ((UINT32_C(1) << size) - 1u) << shift;
}

static uint32_t crash_channel_value(uint8_t component, uint8_t size, uint8_t shift) {
	uint32_t maximum = (UINT32_C(1) << size) - 1u;
	return ((uint32_t)component * maximum + 127u) / 255u << shift;
}

static uint32_t crash_pixel_value(const struct boot_framebuffer* framebuffer, uint8_t red, uint8_t green,
                                  uint8_t blue) {
	return crash_channel_value(red, framebuffer->red_mask_size, framebuffer->red_mask_shift) |
	       crash_channel_value(green, framebuffer->green_mask_size, framebuffer->green_mask_shift) |
	       crash_channel_value(blue, framebuffer->blue_mask_size, framebuffer->blue_mask_shift);
}

static bool crash_framebuffer_layout(const struct boot_framebuffer*   framebuffer,
                                     struct crash_framebuffer_layout* layout) {
	size_t   bytes_per_pixel;
	size_t   visible_row_size;
	size_t   framebuffer_size;
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;

	if (framebuffer == NULL || layout == NULL || framebuffer->address == NULL ||
	    framebuffer->memory_model != CRASH_FRAMEBUFFER_RGB || framebuffer->width == 0u || framebuffer->height == 0u ||
	    framebuffer->width > SIZE_MAX || framebuffer->height > SIZE_MAX || framebuffer->pitch > SIZE_MAX ||
	    framebuffer->bpp < 16u || framebuffer->bpp > 32u || framebuffer->bpp % 8u != 0u)
		return false;
	if (!crash_channel_valid(framebuffer->red_mask_size, framebuffer->red_mask_shift, framebuffer->bpp) ||
	    !crash_channel_valid(framebuffer->green_mask_size, framebuffer->green_mask_shift, framebuffer->bpp) ||
	    !crash_channel_valid(framebuffer->blue_mask_size, framebuffer->blue_mask_shift, framebuffer->bpp))
		return false;
	red_mask   = crash_channel_mask(framebuffer->red_mask_size, framebuffer->red_mask_shift);
	green_mask = crash_channel_mask(framebuffer->green_mask_size, framebuffer->green_mask_shift);
	blue_mask  = crash_channel_mask(framebuffer->blue_mask_size, framebuffer->blue_mask_shift);
	if ((red_mask & green_mask) != 0u || (red_mask & blue_mask) != 0u || (green_mask & blue_mask) != 0u) return false;
	bytes_per_pixel = framebuffer->bpp / 8u;
	if ((size_t)framebuffer->width > SIZE_MAX / bytes_per_pixel) return false;
	visible_row_size = (size_t)framebuffer->width * bytes_per_pixel;
	if ((size_t)framebuffer->pitch < visible_row_size ||
	    (size_t)framebuffer->height > SIZE_MAX / (size_t)framebuffer->pitch)
		return false;
	framebuffer_size = (size_t)framebuffer->height * (size_t)framebuffer->pitch;
	if (framebuffer_size == 0u || (uintptr_t)framebuffer->address > UINTPTR_MAX - (framebuffer_size - 1u)) return false;
	layout->pixels          = (volatile uint8_t*)framebuffer->address;
	layout->width           = (size_t)framebuffer->width;
	layout->height          = (size_t)framebuffer->height;
	layout->pitch           = (size_t)framebuffer->pitch;
	layout->bytes_per_pixel = bytes_per_pixel;
	layout->blue  = crash_pixel_value(framebuffer, CRASH_QR_BLUE_RED, CRASH_QR_BLUE_GREEN, CRASH_QR_BLUE_BLUE);
	layout->white = crash_pixel_value(framebuffer, 0xffu, 0xffu, 0xffu);
	layout->black = crash_pixel_value(framebuffer, 0u, 0u, 0u);
	return true;
}

static void crash_fill_rectangle(const struct crash_framebuffer_layout* layout, size_t left, size_t top, size_t width,
                                 size_t height, uint32_t pixel) {
	for (size_t y = top; y < top + height; y++) {
		volatile uint8_t* row = layout->pixels + y * layout->pitch + left * layout->bytes_per_pixel;
		for (size_t x = 0u; x < width; x++) {
			for (size_t byte = 0u; byte < layout->bytes_per_pixel; byte++)
				row[x * layout->bytes_per_pixel + byte] = (uint8_t)(pixel >> (byte * 8u));
		}
	}
}

static bool crash_qr_module(const struct kernel_crash_qr* qr, size_t x, size_t y) {
	size_t index = y * qr->size + x;
	return ((qr->modules[(index >> 3u) + 1u] >> (index & 7u)) & 1u) != 0u;
}

bool kernel_crash_qr_encode(struct kernel_crash_qr* qr, const char* text, size_t text_size) {
	if (qr == NULL || text == NULL || text_size == 0u || text_size > sizeof(qr->temporary)) return false;
	memcpy(qr->temporary, text, text_size);
	if (!qrcodegen_encodeBinary(qr->temporary,
	                            text_size,
	                            qr->modules,
	                            qrcodegen_Ecc_MEDIUM,
	                            qrcodegen_VERSION_MIN,
	                            qrcodegen_VERSION_MAX,
	                            qrcodegen_Mask_AUTO,
	                            false)) {
		qr->size = 0u;
		return false;
	}
	qr->size = qr->modules[0];
	return qr->size >= 21u && qr->size <= 177u;
}

bool kernel_crash_qr_draw(const struct kernel_crash_qr* qr, const struct boot_framebuffer* framebuffer) {
	struct crash_framebuffer_layout layout;
	size_t                          total_modules;
	size_t                          minimum_dimension;
	size_t                          target_size;
	size_t                          scale;
	size_t                          panel_size;
	size_t                          panel_left;
	size_t                          panel_top;

	if (qr == NULL || qr->size < 21u || qr->size > 177u || qr->modules[0] != qr->size ||
	    !crash_framebuffer_layout(framebuffer, &layout))
		return false;
	total_modules     = (size_t)qr->size + CRASH_QR_QUIET_ZONE * 2u;
	minimum_dimension = layout.width < layout.height ? layout.width : layout.height;
	target_size       = minimum_dimension - minimum_dimension / 8u;
	scale             = target_size / total_modules;
	if (scale < CRASH_QR_MIN_PIXEL_SCALE) return false;
	panel_size = total_modules * scale;
	panel_left = (layout.width - panel_size) / 2u;
	panel_top  = (layout.height - panel_size) / 2u;

	crash_fill_rectangle(&layout, 0u, 0u, layout.width, layout.height, layout.blue);
	crash_fill_rectangle(&layout, panel_left, panel_top, panel_size, panel_size, layout.white);
	for (size_t y = 0u; y < qr->size; y++) {
		for (size_t x = 0u; x < qr->size; x++) {
			if (!crash_qr_module(qr, x, y)) continue;
			crash_fill_rectangle(&layout,
			                     panel_left + (x + CRASH_QR_QUIET_ZONE) * scale,
			                     panel_top + (y + CRASH_QR_QUIET_ZONE) * scale,
			                     scale,
			                     scale,
			                     layout.black);
		}
	}
	return true;
}
