#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "requests.h"

__attribute__((noreturn))
static void hcf(void) {
	printf("kernel: hcf\n");
	__asm__ volatile("cli");
	for (;;) {
		__asm__ volatile("hlt");
	}
}

__attribute__((noreturn))
void kernel_main(void) {
	hal_serial_init();
	printf("kernel: entering kernel_main\n");

	if (!supports_limine_base_revision()) {
		printf("kernel: unsupported limine base revision\n");
		hcf();
	}

	if (!fb_req.response || fb_req.response->framebuffer_count < 1) {
		printf("kernel: framebuffer request failed\n");
		hcf();
	}
	struct limine_framebuffer* fb = fb_req.response->framebuffers[0];

	for (int x = 0; x < (int)fb->width; x++) {
		for (int y = 0; y < (int)fb->height; y++) {
			uint32_t red   = (uint32_t)x * 255 / (uint32_t)fb->width;
			uint32_t green = (uint32_t)y * 255 / (uint32_t)fb->height;
			uint32_t blue  = 64;

			uint8_t*  pixel_addr = (uint8_t*)(uintptr_t)fb->address + (size_t)y * fb->pitch + (size_t)x * (fb->bpp / 8);
			uint32_t* pixel      = (uint32_t*)pixel_addr;
			*pixel               = (red << 16) | (green << 8) | blue;
		}
	}

	if (!memmap_req.response || memmap_req.response->entry_count < 1) {
		hcf();
	}

	printf("kernel: memory map entries:\n");

	uint64_t total_mem = 0;
	for (uint64_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry* entry = memmap_req.response->entries[i];
		if (entry->type == 1) total_mem += entry->length;

		printf("  base: 0x%p, length: 0x%p, type: %u\n",
		       (void*)(uintptr_t)entry->base,
		       (void*)(uintptr_t)entry->length,
		       (unsigned)entry->type);
	}

	printf("kernel: total memory: %u MB\n", (unsigned)(total_mem / (1024 * 1024)));

	hcf();
}
