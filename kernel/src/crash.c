#include <core/cpu.h>
#include <core/pmm.h>
#include <core/thread.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/serial.h>
#include <kernel/crash.h>
#include <string.h>

extern const uint8_t __kernel_build_id_note_start[];
extern const uint8_t __kernel_build_id_note_end[];

struct panic_slot {
	uint32_t                 pending;
	enum kernel_panic_reason reason;
	uintptr_t                detail;
};

static struct kernel_crash_record crash_records[KERNEL_CRASH_MAX_CPUS];
static struct panic_slot          panic_slots[KERNEL_CRASH_MAX_CPUS];
static char                       crash_render_buffer[KERNEL_CRASH_RENDER_SIZE];
static uint32_t                   crash_owner;
static uint64_t                   crash_cpu_count;
static uint64_t                   crash_total_memory;

static size_t crash_cpu_slot(void) {
	struct cpu* cpu = cpu_current();
	return cpu != NULL && cpu->index < KERNEL_CRASH_MAX_CPUS ? cpu->index : 0u;
}

struct kernel_crash_record* kernel_crash_begin(enum kernel_crash_arch arch, enum kernel_crash_cause cause) {
	struct cpu*                 cpu;
	struct kernel_crash_record* record;
	size_t                      slot;

	irq_disable_local();
	slot = crash_cpu_slot();
	if (!kernel_crash_try_claim(&crash_owner, (uint32_t)slot + 1u)) hcf_silent();
	record = &crash_records[slot];
	kernel_crash_record_reset(record, arch, cause);
	cpu = cpu_current();
	if (cpu != NULL) {
		record->cpu_index    = cpu->index;
		record->processor_id = cpu->processor_id;
		record->arch_id      = cpu->arch_id;
	}
	record->cpu_count    = __atomic_load_n(&crash_cpu_count, __ATOMIC_ACQUIRE);
	record->total_memory = __atomic_load_n(&crash_total_memory, __ATOMIC_ACQUIRE);
	record->build_id_valid =
		kernel_crash_parse_build_id(__kernel_build_id_note_start,
		                            (size_t)(__kernel_build_id_note_end - __kernel_build_id_note_start),
		                            record->build_id);
	return record;
}

void kernel_crash_publish_system_metadata(void) {
	uint64_t discovered_cpu_count = (uint64_t)cpu_count();
	uint64_t discovered_memory    = (uint64_t)pmm_total_size();

	__atomic_store_n(&crash_cpu_count, discovered_cpu_count, __ATOMIC_RELEASE);
	__atomic_store_n(&crash_total_memory, discovered_memory, __ATOMIC_RELEASE);
}

bool kernel_crash_take_panic(struct kernel_crash_panic* panic) {
	size_t             slot;
	struct panic_slot* pending;

	if (panic == NULL) return false;
	slot    = crash_cpu_slot();
	pending = &panic_slots[slot];
	if (__atomic_exchange_n(&pending->pending, 0u, __ATOMIC_ACQ_REL) == 0u) return false;
	panic->reason = pending->reason;
	panic->detail = pending->detail;
	return true;
}

__attribute__((noreturn))
void kernel_panic(enum kernel_panic_reason reason, uintptr_t detail) {
	size_t             slot;
	struct panic_slot* pending;

	irq_disable_local();
	slot    = crash_cpu_slot();
	pending = &panic_slots[slot];
	if (__atomic_load_n(&pending->pending, __ATOMIC_ACQUIRE) != 0u) hcf_silent();
	pending->reason = reason;
	pending->detail = detail;
	if (__atomic_exchange_n(&pending->pending, 1u, __ATOMIC_RELEASE) != 0u) hcf_silent();
	__builtin_trap();
	__builtin_unreachable();
}

static void crash_find_stack_bounds(const struct kernel_crash_record* record, uintptr_t* out_base, uintptr_t* out_top) {
	struct cpu*    cpu = cpu_current();
	struct thread* thread;
	uintptr_t      fp = (uintptr_t)record->frame_pointer;

	*out_base = 0u;
	*out_top  = 0u;
	if (cpu == NULL) return;
	thread = cpu_current_thread_load(cpu);
	if (thread != NULL && fp >= thread->kernel_stack_base && fp < thread->kernel_stack_top) {
		*out_base = thread->kernel_stack_base;
		*out_top  = thread->kernel_stack_top;
		return;
	}
	if (fp >= cpu->boot_stack_base && fp < cpu->boot_stack_top) {
		*out_base = cpu->boot_stack_base;
		*out_top  = cpu->boot_stack_top;
	}
}

__attribute__((noreturn))
void kernel_crash_finish(struct kernel_crash_record* record) {
	uintptr_t                      stack_base;
	uintptr_t                      stack_top;
	enum kernel_crash_frame_layout layout;

	if (record == NULL) hcf_silent();
	crash_find_stack_bounds(record, &stack_base, &stack_top);
	layout = record->arch == KERNEL_CRASH_ARCH_RISCV64 || record->arch == KERNEL_CRASH_ARCH_LOONGARCH64
	             ? KERNEL_CRASH_FRAME_RECORD_BEFORE_FP
	             : KERNEL_CRASH_FRAME_RECORD_AT_FP;
	kernel_crash_walk_stack(record, stack_base, stack_top, layout);
	if (kernel_crash_render(record, crash_render_buffer, sizeof(crash_render_buffer)))
		(void)hal_serial_write_emergency(crash_render_buffer, strlen(crash_render_buffer));
	hcf_silent();
}
