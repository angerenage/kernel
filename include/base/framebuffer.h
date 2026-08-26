#pragma once

#include <base/cap.h>
#include <base/vmm.h>
#include <stddef.h>
#include <stdint.h>

/* Operations supported by a kernel framebuffer capability. */
enum framebuffer_op {
	FRAMEBUFFER_OP_INFO = 0,
	FRAMEBUFFER_OP_MAP,
};

/* Common prefix for every framebuffer request. */
struct framebuffer_request_header {
	enum framebuffer_op op;
};

/* Request the framebuffer geometry and pixel format. */
struct framebuffer_info_request {
	struct framebuffer_request_header header;
};

/* Framebuffer byte size, geometry, scanline pitch, and boot-protocol pixel format. */
struct framebuffer_info_response {
	size_t   size;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint16_t bpp;
	uint8_t  memory_model;
	uint8_t  red_mask_size;
	uint8_t  red_mask_shift;
	uint8_t  green_mask_size;
	uint8_t  green_mask_shift;
	uint8_t  blue_mask_size;
	uint8_t  blue_mask_shift;
};

/* Request a read-write mapping of the framebuffer. */
struct framebuffer_map_request {
	struct framebuffer_request_header header;
};

/* Writable caller mapping, its management capability, and the framebuffer's offset from mapping.base. */
struct framebuffer_map_response {
	cap_id_t        mapping_cap;
	struct vmm_info mapping;
	size_t          data_offset;
};
