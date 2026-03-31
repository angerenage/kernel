#include <core/kheap.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/vmm.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/requests.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static __attribute__((noreturn))
void boot_fail(const char* message) {
	printf("%s\n", message);
	hcf();
}

static void boot_log_framebuffer(void) {
	if (!fb_req.response || fb_req.response->framebuffer_count < 1 || !fb_req.response->framebuffers) {
		printf("kernel: no framebuffer available, continuing in headless mode\n");
		return;
	}

	struct limine_framebuffer* fb = fb_req.response->framebuffers[0];
	if (!fb || !fb->address) {
		printf("kernel: framebuffer response invalid, continuing in headless mode\n");
		return;
	}

	printf("kernel: framebuffer available (%ux%u, %u bpp)\n",
	       (unsigned)fb->width,
	       (unsigned)fb->height,
	       (unsigned)fb->bpp);

	for (uint32_t x = 0; x < fb->width; x++) {
		for (uint32_t y = 0; y < fb->height; y++) {
			uint32_t red        = x * 255u / fb->width;
			uint32_t green      = y * 255u / fb->height;
			uint32_t blue       = 64u;
			uint8_t* pixel_addr = (uint8_t*)(uintptr_t)fb->address + (size_t)y * fb->pitch + (size_t)x * (fb->bpp / 8u);
			uint32_t* pixel     = (uint32_t*)pixel_addr;

			*pixel = (red << 16) | (green << 8) | blue;
		}
	}
}

static void boot_log_memory_map(const struct limine_memmap_response* memmap_resp) {
	uint64_t total_mem = 0;

	printf("kernel: memory map entries:\n");
	for (uint64_t i = 0; i < memmap_resp->entry_count; i++) {
		struct limine_memmap_entry* entry      = memmap_resp->entries[i];
		enum mem_range_type         range_type = mem_range_type_from_limine(entry->type);

		if (range_type == MEM_RANGE_USABLE) total_mem += entry->length;

		printf("  base: %p, length: %p, type: %s\n",
		       (void*)(uintptr_t)entry->base,
		       (void*)(uintptr_t)entry->length,
		       mem_range_type_str(range_type));
	}

	printf("kernel: total memory: %u MB\n", (unsigned)(total_mem / (1024 * 1024)));
}

static void kernel_init_memory(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset) {
	if (!pmm_init(memmap_resp, direct_map_offset)) {
		boot_fail("kernel: pmm_init failed");
	}

	printf("kernel: pmm initialized with %zu usable ranges, %zu/%zu pages free\n",
	       pmm_managed_range_count(),
	       pmm_free_page_count(),
	       pmm_total_page_count());

	if (!vmm_init()) {
		boot_fail("kernel: vmm_init failed");
	}

	printf("kernel: vmm initialized for heap window %p (%zu pages)\n", (void*)vmm_heap_base(), vmm_heap_page_count());

	if (!kheap_init()) {
		boot_fail("kernel: kheap_init failed");
	}

	printf("kernel: kheap initialized with %zu/%zu bytes free\n", kheap_free_bytes(), kheap_total_bytes());
}

__attribute__((noreturn))
void kernel_main(void) {
	hal_serial_init();
	printf("kernel: entering kernel_main\n");

	hal_interrupts_init();

	if (!supports_limine_base_revision()) {
		boot_fail("kernel: unsupported limine base revision");
	}
	if (!memmap_req.response || memmap_req.response->entry_count < 1) {
		boot_fail("kernel: memory map request failed");
	}
	if (!hhdm_req.response) {
		boot_fail("kernel: hhdm request failed");
	}

	boot_log_framebuffer();
	boot_log_memory_map(memmap_req.response);
	kernel_init_memory(memmap_req.response, hhdm_req.response->offset);

	hcf();
}
