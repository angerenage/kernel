#include <core/cpu.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <kernel/boot.h>
#include <kernel/cpu_boot.h>
#include <stdio.h>

#define CPU_BOOT_MAX_COUNT 64u
#define CPU_BOOT_STACK_SIZE 0x4000u
#define CPU_AP_START_SPIN_LIMIT 10000000u

static struct cpu_init_info boot_cpu_init[CPU_BOOT_MAX_COUNT];
static _Alignas(16) uint8_t ap_boot_stacks[CPU_BOOT_MAX_COUNT][CPU_BOOT_STACK_SIZE];
static uint8_t cpu_current_ok[CPU_BOOT_MAX_COUNT];

static void kernel_cpu_mp_entry(size_t cpu_index, void* arg) {
	struct cpu* cpu = (struct cpu*)arg;

	(void)cpu_index;
	if (cpu == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}

	kernel_cpu_boot_bind_current(cpu);
	(void)cpu_set_state(cpu, CPU_STATE_STARTING);
	if (!hal_interrupts_init_local(cpu)) {
		for (;;) {
			hal_cpu_park();
		}
	}
	(void)cpu_set_state(cpu, CPU_STATE_ONLINE);
	sched_enter_idle();
}

bool kernel_cpu_boot_init(uintptr_t boot_stack_base, uintptr_t boot_stack_top) {
	size_t cpu_count = 1u;
	size_t bsp_index = 0u;

	for (size_t i = 0; i < CPU_BOOT_MAX_COUNT; i++) {
		cpu_current_ok[i] = 0u;
	}

	if (!kernel_boot_cpu_topology(
			boot_cpu_init, CPU_BOOT_MAX_COUNT, boot_stack_base, boot_stack_top, &cpu_count, &bsp_index)) {
		return false;
	}

	for (size_t i = 0; i < cpu_count; i++) {
		if (boot_cpu_init[i].role != CPU_ROLE_AP) continue;

		boot_cpu_init[i].boot_stack_base = (uintptr_t)ap_boot_stacks[i];
		boot_cpu_init[i].boot_stack_top  = boot_cpu_init[i].boot_stack_base + CPU_BOOT_STACK_SIZE;
	}

	return cpu_topology_init(boot_cpu_init, cpu_count, bsp_index);
}

void kernel_cpu_boot_bind_current(struct cpu* cpu) {
	if (cpu == NULL) return;

	cpu_bind_current(cpu);
	if (cpu->index < CPU_BOOT_MAX_COUNT) {
		__atomic_store_n(&cpu_current_ok[cpu->index], cpu_current() == cpu, __ATOMIC_RELEASE);
	}
}

bool kernel_cpu_boot_start_aps(void) {
	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);

		if (cpu == NULL || cpu->role != CPU_ROLE_AP) continue;

		(void)cpu_set_state(cpu, CPU_STATE_STARTING);
		if (!kernel_boot_cpu_start(cpu->index, kernel_cpu_mp_entry, cpu)) {
			return false;
		}

		for (size_t spin_count = 0; cpu_state_get(cpu) != CPU_STATE_ONLINE; spin_count++) {
			if (spin_count >= CPU_AP_START_SPIN_LIMIT) {
				printf("kernel: AP cpu%zu failed to reach ONLINE (state=%u, bound=%u)\n",
				       cpu->index,
				       (unsigned)cpu_state_get(cpu),
				       kernel_cpu_boot_current_pointer_ok(cpu) ? 1u : 0u);
				return false;
			}
			spinlock_relax();
		}
	}

	return true;
}

bool kernel_cpu_boot_current_pointer_ok(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= CPU_BOOT_MAX_COUNT) return false;
	return __atomic_load_n(&cpu_current_ok[cpu->index], __ATOMIC_ACQUIRE) != 0u;
}
