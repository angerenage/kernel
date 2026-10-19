#pragma once

#include <boot/info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	KERNEL_CRASH_QR_BUFFER_SIZE = 3918u,
};

struct kernel_crash_qr {
	uint8_t  temporary[KERNEL_CRASH_QR_BUFFER_SIZE];
	uint8_t  modules[KERNEL_CRASH_QR_BUFFER_SIZE];
	uint16_t size;
};

/* Encode an explicitly bounded byte string as a medium-correction QR symbol. */
bool kernel_crash_qr_encode(struct kernel_crash_qr* qr, const char* text, size_t text_size);

/* Paint a validated QR symbol over a centered white panel on a blue framebuffer. */
bool kernel_crash_qr_draw(const struct kernel_crash_qr* qr, const struct boot_framebuffer* framebuffer);
