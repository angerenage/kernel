#include <base/heap.h>
#include <base/vmm.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <kernel/boot.h>
#include <kernel/boot_diagnostics.h>
#include <kernel/cmdline.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static struct sched_cpu_stats* scheduler_previous_cpu_stats;
static size_t                  scheduler_previous_cpu_count;
static bool                    scheduler_history_initialized;

static uint64_t diagnostics_counter_delta(uint64_t current, uint64_t previous) {
	return current >= previous ? current - previous : current;
}

static uint64_t diagnostics_tenths_percentage(uint64_t part, uint64_t total) {
	if (total == 0u) return 0u;
	if (part > total) part = total;
	return (part * 1000u + total / 2u) / total;
}

static uint64_t diagnostics_tenths_rate(uint64_t count, uint64_t ticks, uint32_t timer_frequency_hz) {
	if (ticks == 0u || timer_frequency_hz == 0u) return 0u;
	return (count * (uint64_t)timer_frequency_hz * 10u + ticks / 2u) / ticks;
}

static bool diagnostics_prepare_scheduler_history(size_t cpu_total) {
	if (scheduler_previous_cpu_stats != NULL && scheduler_previous_cpu_count == cpu_total) return true;

	free(scheduler_previous_cpu_stats);
	scheduler_previous_cpu_stats  = NULL;
	scheduler_previous_cpu_count  = 0u;
	scheduler_history_initialized = false;
	if (cpu_total == 0u) return false;

	scheduler_previous_cpu_stats = calloc(cpu_total, sizeof(*scheduler_previous_cpu_stats));
	if (scheduler_previous_cpu_stats == NULL) return false;
	scheduler_previous_cpu_count = cpu_total;
	return true;
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
	printf("kernel: memory pmm=%zu/%zu bytes heap=%zu/%zu bytes regions=%zu pages\n",
	       pmm_free_size(),
	       pmm_total_size(),
	       heap_free_bytes(),
	       heap_total_bytes(),
	       MM_KERNEL_VMM_SIZE / VMM_PAGE_SIZE);
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

void kernel_boot_diagnostics_scheduler_report(uint64_t elapsed_ticks, uint32_t timer_frequency_hz) {
	uint64_t elapsed_seconds;
	uint64_t hours;
	uint64_t minutes;
	uint64_t seconds;
	size_t   cpu_total = cpu_count();
	size_t   report_lines;

	if (timer_frequency_hz == 0u) return;
	if (!diagnostics_prepare_scheduler_history(cpu_total)) {
		printf("scheduler: processor statistics unavailable\n");
		return;
	}

	if (!scheduler_history_initialized) {
		for (size_t i = 0; i < cpu_total; i++) {
			struct cpu*            cpu = cpu_by_index(i);
			struct sched_cpu_stats cpu_stats;

			if (cpu == NULL || !sched_get_cpu_stats(cpu, &cpu_stats)) continue;
			scheduler_previous_cpu_stats[i] = cpu_stats;
		}
		scheduler_history_initialized = true;
		return;
	}

	elapsed_seconds = elapsed_ticks / timer_frequency_hz;
	hours           = elapsed_seconds / 3600u;
	minutes         = (elapsed_seconds / 60u) % 60u;
	seconds         = elapsed_seconds % 60u;
	report_lines    = 1u;

	printf("scheduler: uptime %02llu:%02llu:%02llu\n",
	       (unsigned long long)hours,
	       (unsigned long long)minutes,
	       (unsigned long long)seconds);

	for (size_t i = 0; i < cpu_total; i++) {
		struct cpu*             cpu = cpu_by_index(i);
		struct sched_cpu_stats  cpu_stats;
		struct sched_cpu_stats* previous;
		uint64_t                total_ticks;
		uint64_t                thread_ticks;
		uint64_t                kernel_ticks;
		uint64_t                context_switches;
		uint64_t                preemptions;
		uint64_t                yields;
		uint64_t                utilization;
		uint64_t                thread_percentage;
		uint64_t                kernel_percentage;
		uint64_t                context_switch_rate;
		uint64_t                preemption_rate;
		uint64_t                yield_rate;

		if (cpu == NULL || !sched_get_cpu_stats(cpu, &cpu_stats)) continue;

		previous         = &scheduler_previous_cpu_stats[i];
		total_ticks      = diagnostics_counter_delta(cpu_stats.total_ticks, previous->total_ticks);
		thread_ticks     = diagnostics_counter_delta(cpu_stats.thread_ticks, previous->thread_ticks);
		kernel_ticks     = diagnostics_counter_delta(cpu_stats.kernel_ticks, previous->kernel_ticks);
		context_switches = diagnostics_counter_delta(cpu_stats.context_switch_count, previous->context_switch_count);
		preemptions = diagnostics_counter_delta(cpu_stats.timeslice_preempt_count, previous->timeslice_preempt_count);
		yields      = diagnostics_counter_delta(cpu_stats.yield_count, previous->yield_count);

		utilization         = diagnostics_tenths_percentage(thread_ticks + kernel_ticks, total_ticks);
		thread_percentage   = diagnostics_tenths_percentage(thread_ticks, total_ticks);
		kernel_percentage   = diagnostics_tenths_percentage(kernel_ticks, total_ticks);
		context_switch_rate = diagnostics_tenths_rate(context_switches, total_ticks, timer_frequency_hz);
		preemption_rate     = diagnostics_tenths_rate(preemptions, total_ticks, timer_frequency_hz);
		yield_rate          = diagnostics_tenths_rate(yields, total_ticks, timer_frequency_hz);

		printf("\r\033[2Kscheduler: CPU%zu %llu.%llu%% "
		       "(u %llu.%llu%%, k %llu.%llu%%), "
		       "queue %zu, switches %llu/s "
		       "(p %llu/s, y %llu/s)\n",
		       cpu->index,
		       (unsigned long long)(utilization / 10u),
		       (unsigned long long)(utilization % 10u),
		       (unsigned long long)(thread_percentage / 10u),
		       (unsigned long long)(thread_percentage % 10u),
		       (unsigned long long)(kernel_percentage / 10u),
		       (unsigned long long)(kernel_percentage % 10u),
		       sched_run_queue_depth(cpu),
		       (unsigned long long)(context_switch_rate),
		       (unsigned long long)(preemption_rate),
		       (unsigned long long)(yield_rate));

		*previous = cpu_stats;
		report_lines++;
	}

	printf("\033[%zuA\r", report_lines);
}
