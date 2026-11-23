#include <stdio.h>

#include <boot/protocol.h>
#include <hal/cpu.h>
#include <hal/hcf.h>
#include <hal/interrupts.h>
#include <hal/paging.h>
#include <core/cpu.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <kernel/cpu_boot.h>

#define CPU_BOOT_MAX_COUNT 64u
#define CPU_BOOT_STACK_SIZE 0x4000u
#define CPU_AP_DISPATCH_RELAXATION_LIMIT 10000000u
#define CPU_AP_START_TIMEOUT_MS 1000u

static struct cpu_init_info boot_cpu_init[CPU_BOOT_MAX_COUNT];
static _Alignas(16) uint8_t ap_boot_stacks[CPU_BOOT_MAX_COUNT][CPU_BOOT_STACK_SIZE];
static uint8_t cpu_current_ok[CPU_BOOT_MAX_COUNT];
static uint8_t cpu_boot_stack_ok[CPU_BOOT_MAX_COUNT];

static void kernel_cpu_mp_fail(struct cpu* cpu) {
	if (cpu != NULL) (void)cpu_transition_state(cpu, CPU_STATE_STARTING, CPU_STATE_FAILED);
	for (;;) {
		hal_cpu_park();
	}
}

static void kernel_cpu_mp_entry(size_t cpu_index, void* arg) {
	struct cpu* cpu = (struct cpu*)arg;
	uintptr_t stack_marker = (uintptr_t)&cpu;

	(void)cpu_index;
	if (cpu == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}
	if (cpu->index < CPU_BOOT_MAX_COUNT) {
		__atomic_store_n(&cpu_boot_stack_ok[cpu->index],
				stack_marker >= cpu->boot_stack_base && stack_marker < cpu->boot_stack_top,
				__ATOMIC_RELEASE);
	}

	kernel_cpu_boot_bind_current(cpu);
	if (!hal_interrupts_init_local(cpu)) {
		kernel_cpu_mp_fail(cpu);
	}
	irq_enable_local();
	if (!sched_start_cpu(cpu)) {
		kernel_cpu_mp_fail(cpu);
	}
	if (!cpu_transition_state(cpu, CPU_STATE_STARTING, CPU_STATE_ONLINE)) kernel_cpu_mp_fail(cpu);
	sched_enter_idle();
}

static bool kernel_cpu_wait_until_bound(struct cpu* cpu) {
	for (size_t count = 0u; count < CPU_AP_DISPATCH_RELAXATION_LIMIT; count++) {
		if (kernel_cpu_boot_current_pointer_ok(cpu)) return true;
		if (cpu_state_get(cpu) != CPU_STATE_STARTING) return false;
		spinlock_relax();
	}
	return false;
}

bool kernel_cpu_boot_init(uintptr_t boot_stack_base, uintptr_t boot_stack_top) {
	size_t cpu_count = 1u;
	size_t bsp_index = 0u;

	for (size_t i = 0; i < CPU_BOOT_MAX_COUNT; i++) {
		cpu_current_ok[i] = 0u;
		cpu_boot_stack_ok[i] = 0u;
	}

	if (!boot_cpu_topology(
				boot_cpu_init, CPU_BOOT_MAX_COUNT, boot_stack_base, boot_stack_top, &cpu_count, &bsp_index)) {
		return false;
	}

	for (size_t i = 0; i < cpu_count; i++) {
		if (boot_cpu_init[i].role != CPU_ROLE_AP) continue;

		boot_cpu_init[i].boot_stack_base = (uintptr_t)ap_boot_stacks[i];
		boot_cpu_init[i].boot_stack_top = boot_cpu_init[i].boot_stack_base + CPU_BOOT_STACK_SIZE;
	}

	return cpu_topology_init(boot_cpu_init, cpu_count, bsp_index);
}

void kernel_cpu_boot_bind_current(struct cpu* cpu) {
	if (cpu == NULL) return;

	cpu_bind_current(cpu);
	if (!hal_paging_init_local()) hcf();
	if (!hal_cpu_fp_init()) hcf();
	if (cpu->index < CPU_BOOT_MAX_COUNT) {
		__atomic_store_n(&cpu_current_ok[cpu->index], cpu_current() == cpu, __ATOMIC_RELEASE);
	}
}

bool kernel_cpu_boot_start_aps(void) {
	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);

		if (cpu == NULL || cpu->role != CPU_ROLE_AP) continue;

		if (!cpu_transition_state(cpu, CPU_STATE_PRESENT, CPU_STATE_STARTING)) {
			printf("kernel: AP cpu%zu has invalid initial state=%u, excluding it\n",
					cpu->index,
					(unsigned)cpu_state_get(cpu));
			continue;
		}
		if (!boot_cpu_start(cpu->index, kernel_cpu_mp_entry, cpu, cpu->boot_stack_top)) {
			(void)cpu_transition_state(cpu, CPU_STATE_STARTING, CPU_STATE_FAILED);
			printf("kernel: AP cpu%zu launch failed, excluding it\n", cpu->index);
			continue;
		}

		if (!kernel_cpu_wait_until_bound(cpu) ||
				!cpu_wait_until_state(cpu, CPU_STATE_ONLINE, CPU_AP_START_TIMEOUT_MS)) {
			(void)cpu_transition_state(cpu, CPU_STATE_STARTING, CPU_STATE_FAILED);
			if (cpu_state_get(cpu) == CPU_STATE_ONLINE) continue;
			printf("kernel: AP cpu%zu failed to reach ONLINE (state=%u, bound=%u), excluding it\n",
					cpu->index,
					(unsigned)cpu_state_get(cpu),
					kernel_cpu_boot_current_pointer_ok(cpu) ? 1u : 0u);
		}
	}

	return true;
}

bool kernel_cpu_boot_current_pointer_ok(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= CPU_BOOT_MAX_COUNT) return false;
	return __atomic_load_n(&cpu_current_ok[cpu->index], __ATOMIC_ACQUIRE) != 0u;
}

bool kernel_cpu_boot_stack_ok(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= CPU_BOOT_MAX_COUNT) return false;
	return __atomic_load_n(&cpu_boot_stack_ok[cpu->index], __ATOMIC_ACQUIRE) != 0u;
}
