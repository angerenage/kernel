#include <hal/mm.h>
#include <hal/mmu.h>
#include <hal/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "requests.h"

static const char* hal_mem_range_type_str(enum hal_mem_range_type type) {
	switch (type) {
	case HAL_MEM_RANGE_USABLE:
		return "usable";
	case HAL_MEM_RANGE_RESERVED:
		return "reserved";
	case HAL_MEM_RANGE_ACPI_RECLAIMABLE:
		return "acpi_reclaimable";
	case HAL_MEM_RANGE_ACPI_NVS:
		return "acpi_nvs";
	case HAL_MEM_RANGE_BAD_MEMORY:
		return "bad_memory";
	case HAL_MEM_RANGE_BOOTLOADER_RECLAIMABLE:
		return "bootloader_reclaimable";
	case HAL_MEM_RANGE_KERNEL_AND_MODULES:
		return "kernel_and_modules";
	case HAL_MEM_RANGE_FRAMEBUFFER:
		return "framebuffer";
	case HAL_MEM_RANGE_OTHER:
		return "other";
	}
	return "unknown";
}

static enum hal_mem_range_type hal_mem_range_type_from_limine(uint64_t type) {
	switch (type) {
	case LIMINE_MEMMAP_USABLE:
		return HAL_MEM_RANGE_USABLE;
	case LIMINE_MEMMAP_RESERVED:
		return HAL_MEM_RANGE_RESERVED;
	case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
		return HAL_MEM_RANGE_ACPI_RECLAIMABLE;
	case LIMINE_MEMMAP_ACPI_NVS:
		return HAL_MEM_RANGE_ACPI_NVS;
	case LIMINE_MEMMAP_BAD_MEMORY:
		return HAL_MEM_RANGE_BAD_MEMORY;
	case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
		return HAL_MEM_RANGE_BOOTLOADER_RECLAIMABLE;
	case LIMINE_MEMMAP_KERNEL_AND_MODULES:
		return HAL_MEM_RANGE_KERNEL_AND_MODULES;
	case LIMINE_MEMMAP_FRAMEBUFFER:
		return HAL_MEM_RANGE_FRAMEBUFFER;
	default:
		break;
	}

	return HAL_MEM_RANGE_OTHER;
}

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

	if (!hhdm_req.response) {
		printf("kernel: hhdm request failed\n");
		hcf();
	}

	printf("kernel: memory map entries:\n");

	enum { BOOT_MEM_MAX_RANGES = 128 };
	static struct hal_mem_range boot_mem_ranges[BOOT_MEM_MAX_RANGES];
	size_t                      boot_range_count = 0;

	uint64_t total_mem = 0;
	for (uint64_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry* entry      = memmap_req.response->entries[i];
		enum hal_mem_range_type     range_type = hal_mem_range_type_from_limine(entry->type);

		if (range_type == HAL_MEM_RANGE_USABLE) {
			total_mem += entry->length;
		}

		if (boot_range_count < BOOT_MEM_MAX_RANGES) {
			boot_mem_ranges[boot_range_count++] = (struct hal_mem_range){
				.base   = entry->base,
				.length = entry->length,
				.type   = range_type,
			};
		}

		printf("  base: 0x%p, length: 0x%p, type: %s\n",
		       (void*)(uintptr_t)entry->base,
		       (void*)(uintptr_t)entry->length,
		       hal_mem_range_type_str(range_type));
	}

	if (memmap_req.response->entry_count > BOOT_MEM_MAX_RANGES) {
		printf("kernel: warning: truncated memory map to first %u entries\n", (unsigned)BOOT_MEM_MAX_RANGES);
	}

	printf("kernel: total memory: %u MB\n", (unsigned)(total_mem / (1024 * 1024)));

	struct hal_mm_boot_info mm_info = {
		.ranges            = boot_mem_ranges,
		.range_count       = boot_range_count,
		.direct_map_offset = hhdm_req.response->offset,
	};

	if (!hal_mm_init(&mm_info)) {
		printf("kernel: hal_mm_init failed\n");
		hcf();
	}

	if (!hal_mmu_init()) {
		printf("kernel: hal_mmu_init failed\n");
		hcf();
	}

	void* early_page = hal_mm_early_alloc(4096, 4096);
	if (!early_page) {
		printf("kernel: hal_mm_early_alloc failed\n");
		hcf();
	}

	printf("kernel: early allocator reserved page at %p (phys 0x%p)\n",
	       early_page,
	       (void*)hal_mm_virt_to_phys((uintptr_t)early_page));

	uintptr_t early_page_phys = hal_mm_virt_to_phys((uintptr_t)early_page);
	uintptr_t test_map_virt   = 0xffff900000000000ull;
	if (!hal_mmu_map(test_map_virt, early_page_phys, HAL_MMU_PAGE_SIZE, HAL_MMU_FLAG_WRITE | HAL_MMU_FLAG_GLOBAL)) {
		printf("kernel: hal_mmu_map failed for test mapping\n");
		hcf();
	}

	uint64_t* test_page = (uint64_t*)test_map_virt;
	*test_page          = 0xfeedfacecafebeefull;
	printf("kernel: test mapping at %p holds 0x%llx\n", (void*)test_map_virt, (unsigned long long)(*test_page));

	hcf();
}
