#include <core/early_alloc.h>
#include <core/mm.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noreturn))
void kernel_main(void) {
	hal_serial_init();
	printf("kernel: entering kernel_main\n");

	hal_interrupts_init();

	if (!supports_limine_base_revision()) {
		printf("kernel: unsupported limine base revision\n");
		hcf();
	}

	if (!fb_req.response || fb_req.response->framebuffer_count < 1 || !fb_req.response->framebuffers) {
		printf("kernel: no framebuffer available, continuing in headless mode\n");
	}
	else {
		struct limine_framebuffer* fb = fb_req.response->framebuffers[0];

		if (!fb || !fb->address) {
			printf("kernel: framebuffer response invalid, continuing in headless mode\n");
		}
		else {
			for (int x = 0; x < (int)fb->width; x++) {
				for (int y = 0; y < (int)fb->height; y++) {
					uint32_t red   = (uint32_t)x * 255 / (uint32_t)fb->width;
					uint32_t green = (uint32_t)y * 255 / (uint32_t)fb->height;
					uint32_t blue  = 64;

					uint8_t* pixel_addr =
						(uint8_t*)(uintptr_t)fb->address + (size_t)y * fb->pitch + (size_t)x * (fb->bpp / 8);
					uint32_t* pixel = (uint32_t*)pixel_addr;
					*pixel          = (red << 16) | (green << 8) | blue;
				}
			}

			printf("kernel: framebuffer initialized (%ux%u, %u bpp)\n",
			       (unsigned)fb->width,
			       (unsigned)fb->height,
			       (unsigned)fb->bpp);
		}
	}

	if (!memmap_req.response || memmap_req.response->entry_count < 1) {
		printf("kernel: memory map request failed\n");
		hcf();
	}

	if (!hhdm_req.response) {
		printf("kernel: hhdm request failed\n");
		hcf();
	}

	printf("kernel: memory map entries:\n");

	uint64_t total_mem = 0;
	for (uint64_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry* entry      = memmap_req.response->entries[i];
		enum mem_range_type         range_type = mem_range_type_from_limine(entry->type);

		if (range_type == MEM_RANGE_USABLE) {
			total_mem += entry->length;
		}

		printf("  base: 0x%p, length: 0x%p, type: %s\n",
		       (void*)(uintptr_t)entry->base,
		       (void*)(uintptr_t)entry->length,
		       mem_range_type_str(range_type));
	}

	printf("kernel: total memory: %u MB\n", (unsigned)(total_mem / (1024 * 1024)));

	if (!early_init(memmap_req.response, hhdm_req.response->offset)) {
		printf("kernel: early_init failed\n");
		hcf();
	}

	hcf();
}
