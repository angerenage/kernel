#include <core/early_alloc.h>
#include <core/mm.h>
#include <hal/hcf.h>
#include <hal/serial.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static bool is_aligned_uintptr(uintptr_t p, size_t align) {
	return (align == 0) ? true : ((p & (align - 1)) == 0);
}

static void dump_bytes(const char* label, const void* p, size_t n) {
	const uint8_t* b = (const uint8_t*)p;
	printf("%s @ %p: ", label, p);
	for (size_t i = 0; i < n; i++) printf("%02x ", (unsigned)b[i]);
	printf("\n");
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

	if (!early_init(memmap_req.response, hhdm_req.response->offset, mem_range_type_from_limine)) {
		printf("kernel: early_init failed\n");
		hcf();
	}

	printf("kernel: testing early_alloc / early_calloc...\n");

	uint64_t before = early_remaining_bytes();
	printf("kernel: early remaining before tests: %llu bytes\n", (unsigned long long)before);

	struct {
		size_t size;
		size_t align;
	} cases[] = {
		{   1,    1},
		{  16,   16},
		{  24,    8},
		{  64,   64},
		{4096, 4096},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t sz = cases[i].size;
		size_t al = cases[i].align;

		uint64_t r0 = early_remaining_bytes();
		void*    p  = early_alloc(sz, al);
		uint64_t r1 = early_remaining_bytes();

		printf("kernel: early_alloc(size=%u, align=%u) -> %p, remaining: %llu -> %llu\n",
		       (unsigned)sz,
		       (unsigned)al,
		       p,
		       (unsigned long long)r0,
		       (unsigned long long)r1);

		if (!p) {
			printf("kernel: ERROR: early_alloc returned NULL\n");
			hcf();
		}
		if (!is_aligned_uintptr((uintptr_t)p, al)) {
			printf("kernel: ERROR: ptr %p not aligned to %u\n", p, (unsigned)al);
			hcf();
		}
		if (r1 > r0) {
			printf("kernel: ERROR: remaining bytes increased after alloc\n");
			hcf();
		}

		// Write a pattern to ensure the memory is writable
		uint8_t* b = (uint8_t*)p;
		for (size_t j = 0; j < sz; j++) b[j] = (uint8_t)(0xA0u + (uint8_t)j);
		if (sz >= 16) dump_bytes("kernel: pattern", p, 16);
	}

	// 2) calloc zero-fill test
	{
		const size_t nmemb = 32;
		const size_t sz    = 8;
		const size_t al    = 16;

		uint64_t r0 = early_remaining_bytes();
		void*    p  = early_calloc(nmemb, sz, al);
		uint64_t r1 = early_remaining_bytes();

		printf("kernel: early_calloc(nmemb=%u, size=%u, align=%u) -> %p, remaining: %llu -> %llu\n",
		       (unsigned)nmemb,
		       (unsigned)sz,
		       (unsigned)al,
		       p,
		       (unsigned long long)r0,
		       (unsigned long long)r1);

		if (!p) {
			printf("kernel: ERROR: early_calloc returned NULL\n");
			hcf();
		}
		if (!is_aligned_uintptr((uintptr_t)p, al)) {
			printf("kernel: ERROR: calloc ptr %p not aligned to %u\n", p, (unsigned)al);
			hcf();
		}

		// Verify zeroed
		const uint8_t* b = (const uint8_t*)p;
		for (size_t i = 0; i < nmemb * sz; i++) {
			if (b[i] != 0) {
				printf("kernel: ERROR: early_calloc not zeroed at byte %u (value=%02x)\n", (unsigned)i, (unsigned)b[i]);
				dump_bytes("kernel: calloc head", p, 32);
				hcf();
			}
		}
		printf("kernel: early_calloc zero-fill OK\n");
	}

	// 3) Allocation until exhaustion
	//    This helps validate "remaining" behavior and allocator stop condition.
	{
		printf("kernel: stress test: allocate 64 KiB blocks until failure...\n");
		const size_t block = 64 * 1024;
		size_t       count = 0;

		while (1) {
			uint64_t r0 = early_remaining_bytes();
			void*    p  = early_alloc(block, 16);
			uint64_t r1 = early_remaining_bytes();

			if (!p) {
				printf("kernel: early_alloc returned NULL after %u blocks, remaining=%llu bytes\n",
				       (unsigned)count,
				       (unsigned long long)r0);
				break;
			}

			// Touch first/last byte to ensure mapped/writable
			uint8_t* b   = (uint8_t*)p;
			b[0]         = 0x5A;
			b[block - 1] = 0xA5;

			count++;

			// Basic monotonic check (allow equality if your allocator rounds w/ bookkeeping)
			if (r1 > r0) {
				printf("kernel: ERROR: remaining increased during stress test (%llu -> %llu)\n",
				       (unsigned long long)r0,
				       (unsigned long long)r1);
				hcf();
			}

			// Avoid spamming: print every 16 blocks
			if ((count & 0xF) == 0) {
				printf("kernel: stress: blocks=%u, remaining=%llu bytes\n", (unsigned)count, (unsigned long long)r1);
			}
		}
	}

	uint64_t after = early_remaining_bytes();
	printf("kernel: early remaining after tests:  %llu bytes\n", (unsigned long long)after);

	if (after > before) {
		printf("kernel: ERROR: remaining increased overall (%llu -> %llu)\n",
		       (unsigned long long)before,
		       (unsigned long long)after);
		hcf();
	}

	printf("kernel: early_alloc / early_calloc tests done\n");

	hcf();
}
