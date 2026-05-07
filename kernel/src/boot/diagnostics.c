#include <base/heap.h>
#include <core/cpu.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/vmm.h>
#include <kernel/boot.h>
#include <kernel/boot_diagnostics.h>
#include <kernel/cmdline.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static void diagnostics_print_tick_duration(uint64_t ticks, uint32_t timer_frequency_hz) {
	uint64_t seconds = 0u;
	uint64_t millis  = 0u;

	if (timer_frequency_hz != 0u) {
		seconds = ticks / timer_frequency_hz;
		millis  = ((ticks % timer_frequency_hz) * 1000u) / timer_frequency_hz;
	}

	printf("%llu.%03llu s", (unsigned long long)seconds, (unsigned long long)millis);
}

bool kernel_boot_diagnostics_enabled(void) {
	const char* loglevel;
	size_t      loglevel_len;

	if (kernel_cmdline_option_enabled("kernel.debug") || kernel_cmdline_option_enabled("debug")) return true;
	if (!kernel_cmdline_option_value("loglevel", &loglevel, &loglevel_len)) return false;
	return kernel_cmdline_value_equals(loglevel, loglevel_len, "debug");
}

void kernel_boot_diagnostics_framebuffer(void) {
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

void kernel_boot_diagnostics_memory_map(const struct mem_range* memory_map, size_t range_count) {
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

void kernel_boot_diagnostics_memory_summary(void) {
	printf("kernel: memory pmm=%zu/%zu pages heap=%zu/%zu bytes vmm=%zu pages\n",
	       pmm_free_page_count(),
	       pmm_total_page_count(),
	       heap_free_bytes(),
	       heap_total_bytes(),
	       vmm_window_page_count());
}

void kernel_boot_diagnostics_modules(void) {
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

void kernel_boot_diagnostics_scheduler_uptime(uint64_t elapsed_seconds, uint32_t timer_frequency_hz,
                                              size_t* report_lines) {
	struct sched_stats stats;
	size_t             cpu_total = cpu_count();

	sched_get_stats(&stats);
	if (report_lines != NULL && *report_lines != 0u) {
		for (size_t i = 0; i < *report_lines; i++) {
			printf("\r\033[2K");
			if (i + 1u != *report_lines) printf("\033[1A");
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
		diagnostics_print_tick_duration(cpu_stats.thread_ticks, timer_frequency_hz);
		printf(" idle=");
		diagnostics_print_tick_duration(cpu_stats.idle_ticks, timer_frequency_hz);
		printf(" sched=");
		diagnostics_print_tick_duration(cpu_stats.kernel_ticks, timer_frequency_hz);
		printf(" cs=%llu preempt=%llu yield=%llu",
		       (unsigned long long)cpu_stats.context_switch_count,
		       (unsigned long long)cpu_stats.timeslice_preempt_count,
		       (unsigned long long)cpu_stats.yield_count);
	}

	if (report_lines != NULL) *report_lines = 1u + cpu_total;
}
