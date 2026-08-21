#include <base/heap.h>
#include <base/startup.h>
#include <core/capability.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/uthread.h>
#include <core/vm_space.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/boot.h>
#include <kernel/boot_diagnostics.h>
#include <kernel/capability.h>
#include <kernel/cpu_boot.h>
#include <kernel/elf_loader.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "capability/loader.h"
#include "capability/serial.h"

#if KERNEL_SELFTESTS_ENABLED
#include <core/kthread.h>

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
static bool              boot_diagnostics_enabled;

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

static bool kernel_launch_init_process(void) {
	const struct kernel_boot_module* module;
	struct kernel_elf_process        loaded  = {0};
	struct init_startup_info         startup = {0};
	struct uthread*                  main_thread;
	struct process_thread_params     thread_params;
	enum kernel_elf_load_result      load_result;
	enum process_thread_spawn_result start_result;
	cap_id_t                         serial_cap;
	cap_id_t                         loader_cap;

	module = kernel_boot_module_find("init.elf");
	if (module == NULL) {
		printf("kernel: init.elf module not found\n");
		return false;
	}

	load_result = kernel_elf_load_process(module, "init", &loaded);
	if (load_result != KERNEL_ELF_LOAD_OK) {
		printf("kernel: init ELF load failed: %s\n", kernel_elf_load_result_string(load_result));
		return false;
	}
	serial_cap = kernel_capability_serial_grant(process_pid(loaded.process));
	if (serial_cap == CAP_ID_INVALID) {
		(void)process_destroy(loaded.process);
		printf("kernel: init serial capability grant failed\n");
		return false;
	}

	loader_cap = kernel_capability_loader_grant(process_pid(loaded.process));
	if (loader_cap == CAP_ID_INVALID) {
		(void)process_destroy(loaded.process);
		printf("kernel: init loader capability grant failed\n");
		return false;
	}

	startup.base = (struct process_startup_info){
		.size            = sizeof(startup),
		.heap_base       = loaded.heap_base,
		.heap_page_count = loaded.heap_page_count,
		.page_size       = PMM_PAGE_SIZE,
		.serial_cap      = serial_cap,
		.init_cap        = CAP_ID_INVALID,
	};
	startup.loader_cap = loader_cap;
	thread_params      = (struct process_thread_params){
			 .name             = "init/main",
			 .user_entry       = loaded.entry,
			 .arg_data         = &startup,
			 .arg_size         = sizeof(startup),
			 .user_stack_pages = UTHREAD_DEFAULT_USER_STACK_PAGES,
			 .preferred_cpu    = cpu_current(),
			 .detached         = false,
    };
	main_thread  = NULL;
	start_result = process_start_main_thread(loaded.process, &main_thread, &thread_params);
	if (start_result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_destroy(loaded.process);
		printf("kernel: init thread start failed: %u\n", (unsigned)start_result);
		return false;
	}

	if (boot_diagnostics_enabled) {
		printf("kernel: launched init pid=%llu entry=%p thread=%llu\n",
		       (unsigned long long)process_pid(loaded.process),
		       (void*)loaded.entry,
		       (unsigned long long)uthread_id(main_thread));
	}
	return true;
}

#if KERNEL_SELFTESTS_ENABLED
static void kernel_selftest_runner_entry(void* arg) {
	(void)arg;

	if (!kernel_selftests_run()) {
		boot_fail("kernel: selftests failed");
	}
}

static void kernel_run_selftests(void) {
	struct kthread*           worker = NULL;
	struct cpu*               cpu    = cpu_current();
	enum kthread_spawn_result result;

	result = kthread_spawn_on_cpu(&worker, "selftest/runner", kernel_selftest_runner_entry, NULL, cpu);
	if (result != KTHREAD_SPAWN_OK) {
		boot_fail("kernel: selftest runner spawn failed");
	}

	/*
	 * kernel_main still runs through the bootstrap CPU's idle scheduler
	 * context, which cannot block on a join queue. Yield cooperatively until
	 * the runner has completed the deferred EXITING -> ZOMBIE transition.
	 */
	while (!thread_is_reap_safe(&worker->thread)) {
		sched_yield();
	}

	if (worker->thread.exit_code != 0u) {
		boot_fail("kernel: selftest runner exited with failure");
	}
	if (!kthread_destroy(worker)) {
		boot_fail("kernel: selftest runner reclaim failed");
	}
}
#endif

static void boot_clock_tick(void* ctx) {
	(void)ctx;

	sched_tick();
	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);

		if (cpu == NULL || cpu == cpu_current() || cpu_state_get(cpu) != CPU_STATE_ONLINE) continue;
		sched_tick_remote(cpu);
	}

	if (!boot_diagnostics_enabled) return;

	boot_timer_ticks++;
	if (boot_timer_frequency_hz == 0u) return;

	if (!boot_timer_started) {
		boot_timer_started          = true;
		boot_timer_origin_ticks     = boot_timer_ticks;
		boot_timer_reported_seconds = 0u;
		kernel_boot_diagnostics_scheduler_uptime(0u, boot_timer_frequency_hz, &boot_timer_report_lines);
		return;
	}

	uint64_t elapsed_seconds = (boot_timer_ticks - boot_timer_origin_ticks) / boot_timer_frequency_hz;
	if (elapsed_seconds != boot_timer_reported_seconds) {
		boot_timer_reported_seconds = elapsed_seconds;
		kernel_boot_diagnostics_scheduler_uptime(elapsed_seconds, boot_timer_frequency_hz, &boot_timer_report_lines);
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

static void kernel_init_memory(const struct mem_range* memory_map, size_t range_count, uintptr_t direct_map_offset) {
	if (!pmm_init(memory_map, range_count, direct_map_offset)) {
		boot_fail("kernel: pmm_init failed");
	}

	if (!vm_init()) {
		boot_fail("kernel: vm_init failed");
	}

	if (!heap_init()) {
		boot_fail("kernel: heap_init failed");
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

	boot_diagnostics_enabled = kernel_boot_diagnostics_enabled();
	if (boot_diagnostics_enabled) {
		printf("kernel: entering kernel_main\n");
		kernel_boot_diagnostics_framebuffer();
		kernel_boot_diagnostics_memory_map(memory_map, memory_map_count);
		kernel_boot_diagnostics_modules();
	}
	kernel_init_memory(memory_map, memory_map_count, boot_address_space.direct_map_offset);
	if (boot_diagnostics_enabled) kernel_boot_diagnostics_memory_summary();
	if (kernel_boot_cpu_mp_supported() && cpu_count() > 1u && !hal_cpu_prepare_smp()) {
		boot_fail("kernel: hal_cpu_prepare_smp failed");
	}
	if (!kernel_boot_cpu_mp_supported()) {
		printf("kernel: SMP boot hooks unavailable on this platform, continuing with the BSP only\n");
	}
	if (!sched_init()) {
		boot_fail("kernel: sched_init failed");
	}
	if (!kernel_cpu_boot_start_aps()) {
		boot_fail("kernel: kernel_cpu_boot_start_aps failed");
	}
	if (!sched_start_cpu(cpu_current())) {
		boot_fail("kernel: sched_start_cpu failed for bootstrap processor");
	}
	if (boot_diagnostics_enabled)
		printf("kernel: cpu topology %zu present, %zu online\n", cpu_count(), cpu_online_count());

	capability_init();
	kernel_capability_init();

#if KERNEL_SELFTESTS_ENABLED
	if (kernel_selftests_requested()) {
		kernel_run_selftests();
	}
#endif

	boot_start_timer_counter();

	if (!kernel_launch_init_process()) {
		boot_fail("kernel: init launch failed");
	}

	sched_enter_idle();
}
