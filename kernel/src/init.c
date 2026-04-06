#include <core/cpu.h>
#include <core/kheap.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/cpu_boot.h>
#include <kernel/requests.h>
#if KERNEL_SELFTESTS_ENABLED
#include <kernel/selftest.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern uint8_t stack_bottom[];
extern uint8_t stack_top[];

static __attribute__((noreturn))
void boot_fail(const char* message) {
	printf("%s\n", message);
	hcf();
}

#define KERNEL_TIMER_HZ 100u

static volatile uint64_t boot_timer_ticks;
static uint64_t          boot_timer_origin_ticks;
static uint64_t          boot_timer_reported_seconds;
static uint32_t          boot_timer_frequency_hz;
static bool              boot_timer_started;

static void boot_clock_tick(void* ctx) {
	(void)ctx;

	boot_timer_ticks++;
}

static void boot_run_timer_counter(void) {
	hal_clock_init();
	if (!hal_clock_start(KERNEL_TIMER_HZ, boot_clock_tick, NULL)) {
		printf("kernel: boot clock unavailable\n");
		return;
	}
	boot_timer_frequency_hz = hal_clock_frequency();
	if (boot_timer_frequency_hz == 0u) {
		printf("kernel: boot clock frequency unavailable\n");
		hal_clock_stop();
		return;
	}

	for (;;) {
		uint64_t ticks = boot_timer_ticks;
		if (ticks == 0u) continue;

		if (!boot_timer_started) {
			boot_timer_started          = true;
			boot_timer_origin_ticks     = ticks;
			boot_timer_reported_seconds = 0u;
			printf("\r\033[2Kkernel: uptime 0 s");
			continue;
		}

		uint64_t elapsed_seconds = (ticks - boot_timer_origin_ticks) / boot_timer_frequency_hz;
		if (elapsed_seconds != boot_timer_reported_seconds) {
			boot_timer_reported_seconds = elapsed_seconds;
			printf("\r\033[2Kkernel: uptime %llu s", elapsed_seconds);
		}
	}
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

	printf("kernel: vmm initialized for window %p (%zu pages)\n", (void*)vmm_window_base(), vmm_window_page_count());

	if (!kheap_init()) {
		boot_fail("kernel: kheap_init failed");
	}

	printf("kernel: kheap initialized with %zu/%zu bytes free\n", kheap_free_bytes(), kheap_total_bytes());
}

__attribute__((noreturn))
void kernel_main(void) {
	hal_serial_init();
	printf("kernel: entering kernel_main\n");

	if (!kernel_cpu_boot_init((uintptr_t)stack_bottom, (uintptr_t)stack_top)) {
		boot_fail("kernel: kernel_cpu_boot_init failed");
	}
	kernel_cpu_boot_bind_current(cpu_bsp());
	(void)cpu_set_state(cpu_bsp(), CPU_STATE_STARTING);

	if (!hal_interrupts_init_global()) {
		boot_fail("kernel: hal_interrupts_init_global failed");
	}
	if (!hal_interrupts_init_local(cpu_current())) {
		boot_fail("kernel: hal_interrupts_init_local failed");
	}
	(void)cpu_set_state(cpu_current(), CPU_STATE_ONLINE);

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
	if (!kernel_cpu_boot_start_aps()) {
		boot_fail("kernel: kernel_cpu_boot_start_aps failed");
	}
	printf("kernel: cpu topology %zu present, %zu online\n", cpu_count(), cpu_online_count());
#if KERNEL_SELFTESTS_ENABLED
	if (kernel_selftests_requested() && !kernel_selftests_run()) {
		boot_fail("kernel: selftests failed");
	}
#endif

	boot_run_timer_counter();

	hcf();
}
