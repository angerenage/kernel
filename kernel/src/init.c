#include <core/cpu.h>
#include <core/kheap.h>
#include <core/kthread.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/boot.h>
#include <kernel/cpu_boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if KERNEL_SELFTESTS_ENABLED
#include "../test/selftest.h"
#endif

extern uint8_t stack_bottom[];
extern uint8_t stack_top[];

__attribute__((noreturn))
static void boot_fail(const char* message) {
	printf("%s\n", message);
	hcf();
}

#define KERNEL_TIMER_HZ 100u
#define KERNEL_BOOTSTRAP_THREAD_STACK_PAGES 4u

static volatile uint64_t boot_timer_ticks;
static uint64_t          boot_timer_origin_ticks;
static uint64_t          boot_timer_reported_seconds;
static uint32_t          boot_timer_frequency_hz;
static bool              boot_timer_started;

static void kernel_bootstrap_worker_entry(void* arg) {
	(void)arg;

	printf("kernel: bootstrap worker running on cpu%zu\n", cpu_index());

	void* block = kmalloc(128u);
	if (block == NULL) {
		printf("kernel: bootstrap worker heap allocation failed\n");
		return;
	}

	printf("kernel: bootstrap worker allocated 128 bytes at %p\n", block);
	kfree(block);
	printf("kernel: bootstrap worker completed\n");
}

static void boot_clock_tick(void* ctx) {
	(void)ctx;

	boot_timer_ticks++;
	if (boot_timer_frequency_hz == 0u) return;

	if (!boot_timer_started) {
		boot_timer_started          = true;
		boot_timer_origin_ticks     = boot_timer_ticks;
		boot_timer_reported_seconds = 0u;
		printf("\r\033[2Kkernel: uptime 0 s");
		return;
	}

	uint64_t elapsed_seconds = (boot_timer_ticks - boot_timer_origin_ticks) / boot_timer_frequency_hz;
	if (elapsed_seconds != boot_timer_reported_seconds) {
		boot_timer_reported_seconds = elapsed_seconds;
		printf("\r\033[2Kkernel: uptime %llu s", elapsed_seconds);
	}
}

static void boot_start_timer_counter(void) {
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
}

static void boot_log_framebuffer(void) {
	struct kernel_boot_framebuffer fb;

	if (!kernel_boot_framebuffer_get(&fb)) {
		printf("kernel: no framebuffer available, continuing in headless mode\n");
		return;
	}

	printf(
		"kernel: framebuffer available (%ux%u, %u bpp)\n", (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.bpp);

	for (uint32_t x = 0; x < fb.width; x++) {
		for (uint32_t y = 0; y < fb.height; y++) {
			uint32_t  red        = x * 255u / (uint32_t)fb.width;
			uint32_t  green      = y * 255u / (uint32_t)fb.height;
			uint32_t  blue       = 64u;
			uint8_t*  pixel_addr = (uint8_t*)fb.address + (size_t)y * fb.pitch + (size_t)x * ((size_t)fb.bpp / 8u);
			uint32_t* pixel      = (uint32_t*)pixel_addr;

			*pixel = (red << 16) | (green << 8) | blue;
		}
	}
}

static void boot_log_memory_map(const struct mem_range* memory_map, size_t range_count) {
	uint64_t total_mem = 0;

	printf("kernel: memory map entries:\n");
	for (size_t i = 0; i < range_count; i++) {
		const struct mem_range* entry = &memory_map[i];

		if (entry->type == MEM_RANGE_USABLE) total_mem += entry->length;

		printf("  base: %p, length: %p, type: %s\n",
		       (void*)entry->base,
		       (void*)(uintptr_t)entry->length,
		       mem_range_type_str(entry->type));
	}

	printf("kernel: total memory: %u MB\n", (unsigned)(total_mem / (1024 * 1024)));
}

static void kernel_init_memory(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset) {
	if (!pmm_init(memory_map, range_count, direct_map_offset)) {
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

static void kernel_run_bootstrap_worker(void) {
	struct vmm_alloc_params stack_params = {
		.page_count  = KERNEL_BOOTSTRAP_THREAD_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct thread_create_params thread_params;
	struct thread               worker;
	struct cpu*                 cpu      = cpu_current();
	vmm_id_t                    stack_id = VMM_ID_INVALID;
	void*                       stack_base;

	if (!sched_start_cpu(cpu)) {
		boot_fail("kernel: sched_start_cpu failed for bootstrap worker");
	}

	if (!vmm_alloc(&stack_params, &stack_id, &stack_base)) {
		boot_fail("kernel: bootstrap worker stack allocation failed");
	}

	thread_params = (struct thread_create_params){
		.name              = "bootstrap/worker",
		.entry             = kernel_bootstrap_worker_entry,
		.arg               = NULL,
		.kernel_stack_base = (uintptr_t)stack_base,
		.kernel_stack_top  = (uintptr_t)stack_base + KERNEL_BOOTSTRAP_THREAD_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE,
		.preferred_cpu     = cpu,
		.detached          = false,
	};

	if (!kthread_create(&worker, &thread_params)) {
		printf("kernel: runtime thread bootstrap not implemented on this platform yet\n");
		(void)vmm_free(stack_id);
		return;
	}
	if (!kthread_start(&worker)) {
		boot_fail("kernel: bootstrap worker failed to start");
	}

	printf("kernel: starting bootstrap worker on cpu%zu\n", cpu->index);
	sched_yield();

	if (!thread_is_terminated(&worker)) {
		boot_fail("kernel: bootstrap worker returned without exiting");
	}
	printf("kernel: bootstrap worker exited with code %llu\n", (unsigned long long)worker.exit_code);

	if (!vmm_free(stack_id)) {
		boot_fail("kernel: bootstrap worker stack reclaim failed");
	}
}

__attribute__((noreturn))
void kernel_main(void) {
	size_t                           memory_map_count = 0u;
	const struct mem_range*          memory_map       = NULL;
	struct kernel_boot_address_space boot_address_space;

	if (!kernel_boot_init()) {
		hal_serial_init();
		boot_fail("kernel: kernel_boot_init failed");
	}

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

	if (!kernel_boot_protocol_supported()) boot_fail("kernel: boot protocol unavailable");
	memory_map = kernel_boot_memmap(&memory_map_count);
	if (memory_map == NULL || memory_map_count == 0u) boot_fail("kernel: memory map unavailable");
	if (!kernel_boot_address_space_get(&boot_address_space)) boot_fail("kernel: boot address space unavailable");

	boot_log_framebuffer();
	boot_log_memory_map(memory_map, memory_map_count);
	kernel_init_memory(memory_map, memory_map_count, boot_address_space.direct_map_offset);
	if (!kernel_boot_cpu_mp_supported()) {
		printf("kernel: SMP boot hooks unavailable on this platform, continuing with the BSP only\n");
	}
	if (!sched_init()) {
		boot_fail("kernel: sched_init failed");
	}
	if (!kernel_cpu_boot_start_aps()) {
		boot_fail("kernel: kernel_cpu_boot_start_aps failed");
	}
	printf("kernel: cpu topology %zu present, %zu online\n", cpu_count(), cpu_online_count());
	kernel_run_bootstrap_worker();
#if KERNEL_SELFTESTS_ENABLED
	if (kernel_selftests_requested() && !kernel_selftests_run()) {
		boot_fail("kernel: selftests failed");
	}
#endif

	boot_start_timer_counter();
	sched_enter_idle();
}
