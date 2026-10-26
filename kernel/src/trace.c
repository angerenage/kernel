#include <kernel/trace.h>

#if KERNEL_TRACE_ENABLED

#include <boot/info.h>
#include <core/cpu.h>
#include <core/kthread.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <hal/clock.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/cmdline.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef KERNEL_TRACE_DURATION_MS
#define KERNEL_TRACE_DURATION_MS 5000u
#endif

#ifndef KERNEL_TRACE_BUFFER_KB
#define KERNEL_TRACE_BUFFER_KB 256u
#endif

#define KERNEL_TRACE_FLAG_FINALIZED (1u << 0)
#define KERNEL_TRACE_FLAG_VIRTUAL_TSC (1u << 1)

enum kernel_trace_transport {
	KERNEL_TRACE_TRANSPORT_QEMU = 0,
	KERNEL_TRACE_TRANSPORT_SERIAL,
};

static struct kernel_trace_file_header* trace_header;
static uintptr_t                        trace_physical_address;
static size_t                           trace_allocation_size;
static uint64_t                         trace_duration_ms;
static uint32_t                         trace_writers;
static bool                             trace_active;
static enum kernel_trace_transport      trace_transport;
static bool                             trace_virtual_tsc;

#if defined(PLATFORM_PC_X86_64)
static bool trace_x86_hypervisor_present(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
	return (ecx & (1u << 31u)) != 0u;
}

static uint64_t trace_x86_read_tsc(void) {
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(low), "=d"(high) : : "memory");
	return ((uint64_t)high << 32u) | low;
}
#endif

static bool trace_counter_read(uint64_t* out) {
#if defined(PLATFORM_PC_X86_64)
	if (trace_virtual_tsc) {
		if (out == NULL) return false;
		*out = trace_x86_read_tsc();
		return true;
	}
#endif
	return hal_clock_monotonic_counter(out);
}

static bool trace_parse_u64(const char* value, size_t length, uint64_t* out) {
	uint64_t parsed = 0u;

	if (value == NULL || length == 0u || out == NULL) return false;
	for (size_t i = 0u; i < length; i++) {
		uint8_t digit;
		if (value[i] < '0' || value[i] > '9') return false;
		digit = (uint8_t)(value[i] - '0');
		if (parsed > (UINT64_MAX - digit) / 10u) return false;
		parsed = parsed * 10u + digit;
	}
	if (parsed == 0u) return false;
	*out = parsed;
	return true;
}

static size_t trace_align_up(size_t value, size_t alignment) {
	if (alignment == 0u || value > SIZE_MAX - (alignment - 1u)) return 0u;
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t trace_crc32(const uint8_t* data, size_t size) {
	uint32_t crc = UINT32_MAX;

	for (size_t i = 0u; i < size; i++) {
		crc ^= data[i];
		for (unsigned bit = 0u; bit < 8u; bit++) {
			uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
			crc           = (crc >> 1u) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}

static enum kernel_trace_transport trace_selected_transport(void) {
	const char* value;
	size_t      length;

	if (!kernel_cmdline_option_value("kernel.trace.transport", &value, &length) || value == NULL) {
		return KERNEL_TRACE_TRANSPORT_QEMU;
	}
	if (kernel_cmdline_value_equals(value, length, "serial")) return KERNEL_TRACE_TRANSPORT_SERIAL;
	return KERNEL_TRACE_TRANSPORT_QEMU;
}

bool kernel_trace_init(void) {
	const struct pmm_info* properties;
	struct pmm_extent      allocation;
	const char*            duration_value;
	size_t                 duration_length;
	size_t                 count;
	size_t                 per_cpu_size;
	size_t                 total_size;
	uint64_t               frequency;
	uint64_t               now = 0u;

	if (!kernel_cmdline_option_enabled("kernel.trace") || trace_header != NULL) return false;
	properties = pmm_info();
	frequency  = hal_clock_monotonic_frequency();
	count      = cpu_count();
#if defined(PLATFORM_PC_X86_64)
	if (frequency == 0u) {
		const struct boot_info* info = boot_info_get();
		if (info != NULL && info->monotonic_counter_frequency_hz != 0u && trace_x86_hypervisor_present()) {
			frequency         = info->monotonic_counter_frequency_hz;
			trace_virtual_tsc = true;
		}
	}
#endif
	if (properties == NULL || properties->allocation_granule == 0u || frequency == 0u || count == 0u ||
	    count > KERNEL_TRACE_MAX_CPUS) {
		const struct boot_info* info = boot_info_get();
		printf("ktrace: prerequisites unavailable counter_hz=%llu boot_counter_hz=%llu cpus=%zu\n",
		       (unsigned long long)frequency,
		       (unsigned long long)(info == NULL ? 0u : info->monotonic_counter_frequency_hz),
		       count);
		return false;
	}

	per_cpu_size = trace_align_up((size_t)KERNEL_TRACE_BUFFER_KB * 1024u, sizeof(struct kernel_trace_record));
	if (per_cpu_size == 0u || count > (SIZE_MAX - sizeof(struct kernel_trace_file_header)) / per_cpu_size) return false;
	total_size =
		trace_align_up(sizeof(struct kernel_trace_file_header) + count * per_cpu_size, properties->allocation_granule);
	if (total_size == 0u ||
	    !pmm_alloc(&(const struct pmm_alloc_request){.size = total_size, .alignment = properties->allocation_granule},
	               &allocation))
		return false;

	trace_header           = (void*)(allocation.address + boot_info.direct_map_offset);
	trace_physical_address = allocation.address;
	trace_allocation_size  = allocation.size;
	memset(trace_header, 0, trace_allocation_size);
	(void)trace_counter_read(&now);
	*trace_header = (struct kernel_trace_file_header){
		.magic           = KERNEL_TRACE_MAGIC,
		.version         = KERNEL_TRACE_VERSION,
		.header_size     = sizeof(struct kernel_trace_file_header),
		.total_size      = trace_allocation_size,
		.clock_frequency = frequency,
		.record_size     = sizeof(struct kernel_trace_record),
		.cpu_count       = (uint32_t)count,
		.session_id      = now ^ allocation.address ^ ((uint64_t)count << 48u),
		.flags           = trace_virtual_tsc ? KERNEL_TRACE_FLAG_VIRTUAL_TSC : 0u,
	};
	for (size_t i = 0u; i < count; i++) {
		trace_header->cpus[i].offset   = sizeof(struct kernel_trace_file_header) + i * per_cpu_size;
		trace_header->cpus[i].capacity = per_cpu_size / sizeof(struct kernel_trace_record);
	}

	trace_duration_ms = KERNEL_TRACE_DURATION_MS;
	if (kernel_cmdline_option_value("kernel.trace.duration_ms", &duration_value, &duration_length)) {
		(void)trace_parse_u64(duration_value, duration_length, &trace_duration_ms);
	}
	trace_transport = trace_selected_transport();
	__atomic_store_n(&trace_active, true, __ATOMIC_RELEASE);
	printf("ktrace: recording session=%llx cpus=%zu buffer_kb=%u duration_ms=%llu transport=%s\n",
	       (unsigned long long)trace_header->session_id,
	       count,
	       (unsigned)KERNEL_TRACE_BUFFER_KB,
	       (unsigned long long)trace_duration_ms,
	       trace_transport == KERNEL_TRACE_TRANSPORT_SERIAL ? "serial" : "qemu");
	return true;
}

uint64_t kernel_trace_thread_id(const struct thread* thread) {
	return (uint64_t)(uintptr_t)thread;
}

void kernel_trace_emit(uint32_t event_id, enum kernel_trace_record_type type, uint64_t arg0, uint64_t arg1) {
	struct kernel_trace_cpu_descriptor* descriptor;
	struct kernel_trace_record*         records;
	struct irq_state                    irq_state;
	uint64_t                            index;
	uint64_t                            timestamp;
	size_t                              cpu;

	if (!__atomic_load_n(&trace_active, __ATOMIC_ACQUIRE)) return;
	irq_state = irq_save_disable();
	(void)__atomic_fetch_add(&trace_writers, 1u, __ATOMIC_ACQ_REL);
	if (!__atomic_load_n(&trace_active, __ATOMIC_ACQUIRE) || !trace_counter_read(&timestamp)) {
		(void)__atomic_fetch_sub(&trace_writers, 1u, __ATOMIC_RELEASE);
		irq_restore(irq_state);
		return;
	}

	cpu = cpu_index();
	if (trace_header == NULL || cpu >= trace_header->cpu_count) {
		(void)__atomic_fetch_sub(&trace_writers, 1u, __ATOMIC_RELEASE);
		irq_restore(irq_state);
		return;
	}
	descriptor = &trace_header->cpus[cpu];
	index      = __atomic_load_n(&descriptor->committed, __ATOMIC_RELAXED);
	if (index >= descriptor->capacity) {
		(void)__atomic_fetch_add(&descriptor->dropped, 1u, __ATOMIC_RELAXED);
	}
	else {
		records        = (void*)((uint8_t*)trace_header + descriptor->offset);
		records[index] = (struct kernel_trace_record){
			.timestamp = timestamp,
			.arg0      = arg0,
			.arg1      = arg1,
			.event_id  = event_id,
			.cpu       = (uint16_t)cpu,
			.type      = (uint8_t)type,
		};
		__atomic_store_n(&descriptor->committed, index + 1u, __ATOMIC_RELEASE);
	}
	(void)__atomic_fetch_sub(&trace_writers, 1u, __ATOMIC_RELEASE);
	irq_restore(irq_state);
}

void kernel_trace_finalize(void) {
	bool     expected = true;
	uint32_t checksum;

	if (!__atomic_compare_exchange_n(&trace_active, &expected, false, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	while (__atomic_load_n(&trace_writers, __ATOMIC_ACQUIRE) != 0u) spinlock_relax();
	trace_header->flags |= KERNEL_TRACE_FLAG_FINALIZED;
	trace_header->checksum = 0u;
	checksum               = trace_crc32((const uint8_t*)trace_header, trace_allocation_size);
	trace_header->checksum = checksum;
	if (trace_transport == KERNEL_TRACE_TRANSPORT_SERIAL) {
		printf("KTRACE_STREAM_BEGIN version=%u session=%llx size=%zu crc32=%08x\n",
		       KERNEL_TRACE_VERSION,
		       (unsigned long long)trace_header->session_id,
		       trace_allocation_size,
		       checksum);
		hal_serial_write((const char*)trace_header, trace_allocation_size);
		printf("\nKTRACE_STREAM_END session=%llx\n", (unsigned long long)trace_header->session_id);
	}
	else {
		printf("KTRACE_READY version=%u session=%llx phys=0x%llx size=%zu crc32=%08x\n",
		       KERNEL_TRACE_VERSION,
		       (unsigned long long)trace_header->session_id,
		       (unsigned long long)trace_physical_address,
		       trace_allocation_size,
		       checksum);
	}
}

static void trace_autostop_entry(void* arg) {
	(void)arg;
	if (kthread_sleep_ms(trace_duration_ms)) kernel_trace_finalize();
}

bool kernel_trace_start_autostop(void) {
	if (!__atomic_load_n(&trace_active, __ATOMIC_ACQUIRE)) return false;
	return kthread_spawn_detached("kernel/trace-stop", trace_autostop_entry, NULL) == KTHREAD_SPAWN_OK;
}

#endif
