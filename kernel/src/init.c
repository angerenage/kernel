#include <base/heap.h>
#include <core/cpu.h>
#include <core/kthread.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/uthread.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/boot.h>
#include <kernel/cpu_boot.h>
#include <kernel/elf_loader.h>
#include <libc/stdlib.h>
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

static volatile uint64_t boot_timer_ticks;
static uint64_t          boot_timer_origin_ticks;
static uint64_t          boot_timer_reported_seconds;
static size_t            boot_timer_report_lines;
static uint32_t          boot_timer_frequency_hz;
static bool              boot_timer_started;

static void boot_print_tick_duration(uint64_t ticks) {
	uint64_t seconds = 0u;
	uint64_t millis  = 0u;

	if (boot_timer_frequency_hz != 0u) {
		seconds = ticks / boot_timer_frequency_hz;
		millis  = ((ticks % boot_timer_frequency_hz) * 1000u) / boot_timer_frequency_hz;
	}

	printf("%llu.%03llu s", (unsigned long long)seconds, (unsigned long long)millis);
}

static const char* kernel_elf_load_result_string(enum kernel_elf_load_result result) {
	switch (result) {
	case KERNEL_ELF_LOAD_OK:
		return "ok";
	case KERNEL_ELF_LOAD_INVALID_ARGUMENTS:
		return "invalid arguments";
	case KERNEL_ELF_LOAD_BAD_FORMAT:
		return "bad format";
	case KERNEL_ELF_LOAD_UNSUPPORTED:
		return "unsupported";
	case KERNEL_ELF_LOAD_NO_MEMORY:
		return "no memory";
	case KERNEL_ELF_LOAD_MAP_FAILED:
		return "map failed";
	case KERNEL_ELF_LOAD_COPY_FAILED:
		return "copy failed";
	case KERNEL_ELF_LOAD_START_FAILED:
		return "start failed";
	}
	return "unknown";
}

static void kernel_launch_init_process(void) {
	const struct kernel_boot_module* module;
	struct kernel_elf_process        loaded = {0};
	struct uthread*                  main_thread;
	enum kernel_elf_load_result      load_result;
	enum process_thread_spawn_result start_result;

	module = kernel_boot_module_find("init.elf");
	if (module == NULL) {
		printf("kernel: init.elf module not found\n");
		return;
	}

	load_result = kernel_elf_load_process(module, "init", &loaded);
	if (load_result != KERNEL_ELF_LOAD_OK) {
		printf("kernel: init ELF load failed: %s\n", kernel_elf_load_result_string(load_result));
		return;
	}

	main_thread  = NULL;
	start_result = process_start_main_thread(loaded.process,
	                                         &main_thread,
	                                         &(const struct process_thread_params){
												 .name             = "init/main",
												 .user_entry       = loaded.entry,
												 .user_arg         = 0u,
												 .user_stack_pages = UTHREAD_DEFAULT_USER_STACK_PAGES,
												 .preferred_cpu    = cpu_current(),
												 .detached         = false,
											 });
	if (start_result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_destroy(loaded.process);
		printf("kernel: init thread start failed: %u\n", (unsigned)start_result);
		return;
	}

	printf("kernel: launched init pid=%llu entry=%p thread=%llu\n",
	       (unsigned long long)process_pid(loaded.process),
	       (void*)loaded.entry,
	       (unsigned long long)uthread_id(main_thread));
}

static void boot_log_scheduler_uptime(uint64_t elapsed_seconds) {
	struct sched_stats stats;
	size_t             cpu_total = cpu_count();

	sched_get_stats(&stats);
	if (boot_timer_report_lines != 0u) {
		for (size_t i = 0; i < boot_timer_report_lines; i++) {
			printf("\r\033[2K");
			if (i + 1u != boot_timer_report_lines) printf("\033[1A");
		}
		printf("\r");
	}

	printf("kernel: uptime %llu s [sched cs=%llu preempt=%llu yield=%llu]",
	       (unsigned long long)elapsed_seconds,
	       (unsigned long long)stats.context_switch_count,
	       (unsigned long long)stats.timeslice_preempt_count,
	       (unsigned long long)stats.yield_count);
	for (size_t i = 0; i < cpu_total; i++) {
		struct cpu*            cpu = cpu_by_index(i);
		struct sched_cpu_stats cpu_stats;

		if (cpu == NULL || !sched_get_cpu_stats(cpu, &cpu_stats)) continue;

		printf("\n  cpu%zu: run=", cpu->index);
		boot_print_tick_duration(cpu_stats.thread_ticks);
		printf(" idle=");
		boot_print_tick_duration(cpu_stats.idle_ticks);
		printf(" sched=");
		boot_print_tick_duration(cpu_stats.kernel_ticks);
		printf(" cs=%llu preempt=%llu yield=%llu",
		       (unsigned long long)cpu_stats.context_switch_count,
		       (unsigned long long)cpu_stats.timeslice_preempt_count,
		       (unsigned long long)cpu_stats.yield_count);
	}

	boot_timer_report_lines = 1u + cpu_total;
}

static void kernel_bootstrap_worker_entry(void* arg) {
	(void)arg;

	printf("kernel: bootstrap worker running on cpu%zu\n", cpu_index());

	void* block = malloc(128u);
	if (block == NULL) {
		printf("kernel: bootstrap worker heap allocation failed\n");
		return;
	}

	printf("kernel: bootstrap worker allocated 128 bytes at %p\n", block);
	free(block);

#if KERNEL_SELFTESTS_ENABLED
	if (kernel_selftests_requested() && !kernel_selftests_run()) {
		boot_fail("kernel: selftests failed");
	}
#endif

	kernel_launch_init_process();
	sched_yield();

	printf("kernel: bootstrap worker completed\n");
}

static void boot_clock_tick(void* ctx) {
	(void)ctx;

	sched_tick();
	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);

		if (cpu == NULL || cpu == cpu_current() || cpu_state_get(cpu) != CPU_STATE_ONLINE) continue;
		sched_tick_remote(cpu);
	}
	boot_timer_ticks++;
	if (boot_timer_frequency_hz == 0u) return;

	if (!boot_timer_started) {
		boot_timer_started          = true;
		boot_timer_origin_ticks     = boot_timer_ticks;
		boot_timer_reported_seconds = 0u;
		boot_log_scheduler_uptime(0u);
		return;
	}

	uint64_t elapsed_seconds = (boot_timer_ticks - boot_timer_origin_ticks) / boot_timer_frequency_hz;
	if (elapsed_seconds != boot_timer_reported_seconds) {
		boot_timer_reported_seconds = elapsed_seconds;
		boot_log_scheduler_uptime(elapsed_seconds);
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

static void boot_log_modules(void) {
	size_t module_count = kernel_boot_module_count();

	if (module_count == 0u) {
		printf("kernel: no boot modules loaded\n");
		return;
	}

	printf("kernel: boot modules:\n");
	for (size_t i = 0; i < module_count; i++) {
		const struct kernel_boot_module* module = kernel_boot_module_at(i);

		if (module == NULL) continue;
		printf("  name: %s, path: %s, address: %p, size: %zu bytes\n",
		       module->name != NULL ? module->name : "(none)",
		       module->path != NULL ? module->path : "(none)",
		       module->address,
		       module->size);
	}
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

	if (!heap_init()) {
		boot_fail("kernel: heap_init failed");
	}

	printf("kernel: heap initialized with %zu/%zu bytes free\n", heap_free_bytes(), heap_total_bytes());
}

static void kernel_bootstrap_worker_handle_spawn_failure(enum kthread_spawn_result result) {
	switch (result) {
	case KTHREAD_SPAWN_CONTEXT_UNSUPPORTED:
		printf("kernel: bootstrap worker context setup rejected by hal_cpu_thread_context_init\n");
#if KERNEL_THREAD_BOOTSTRAP_WARN_FALLBACK
		printf("kernel: continuing without runtime thread bootstrap because warn fallback is enabled\n");
		return;
#else
		boot_fail("kernel: runtime thread bootstrap unsupported on this platform");
#endif
	case KTHREAD_SPAWN_INVALID_ARGUMENTS:
	case KTHREAD_SPAWN_NO_MEMORY:
	case KTHREAD_SPAWN_STACK_ALLOC_FAILED:
	case KTHREAD_SPAWN_START_FAILED:
	case KTHREAD_SPAWN_REAPER_UNAVAILABLE:
	case KTHREAD_SPAWN_OK:
	default:
		boot_fail("kernel: bootstrap worker spawn failed");
	}
}

static void kernel_run_bootstrap_worker(void) {
	struct kthread*           worker = NULL;
	struct cpu*               cpu    = cpu_current();
	enum kthread_spawn_result result;

	if (!sched_start_cpu(cpu)) {
		boot_fail("kernel: sched_start_cpu failed for bootstrap worker");
	}

	result = kthread_spawn_on_cpu(&worker, "bootstrap/worker", kernel_bootstrap_worker_entry, NULL, cpu);
	if (result != KTHREAD_SPAWN_OK) {
		kernel_bootstrap_worker_handle_spawn_failure(result);
		return;
	}

	printf("kernel: starting bootstrap worker on cpu%zu\n", cpu->index);
	sched_yield();

	if (!thread_is_terminated(&worker->thread)) {
		boot_fail("kernel: bootstrap worker returned without exiting");
	}
	printf("kernel: bootstrap worker exited with code %llu\n", (unsigned long long)worker->thread.exit_code);

	if (!kthread_destroy(worker)) {
		boot_fail("kernel: bootstrap worker reclaim failed");
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
	irq_enable_local();
	(void)cpu_set_state(cpu_current(), CPU_STATE_ONLINE);

	if (!kernel_boot_protocol_supported()) boot_fail("kernel: boot protocol unavailable");
	memory_map = kernel_boot_memmap(&memory_map_count);
	if (memory_map == NULL || memory_map_count == 0u) boot_fail("kernel: memory map unavailable");
	if (!kernel_boot_address_space_get(&boot_address_space)) boot_fail("kernel: boot address space unavailable");

	boot_log_framebuffer();
	boot_log_memory_map(memory_map, memory_map_count);
	boot_log_modules();
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

	boot_start_timer_counter();
	sched_enter_idle();
}
