#include <core/cpu.h>
#include <core/spinlock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <kernel/cpu_boot.h>
#include <kernel/requests.h>
#include <limine.h>
#include <stdio.h>

#define CPU_BOOT_MAX_COUNT 64u
#define CPU_BOOT_STACK_SIZE 0x4000u
#define CPU_AP_START_SPIN_LIMIT 10000000u

static struct cpu_init_info boot_cpu_init[CPU_BOOT_MAX_COUNT];
static _Alignas(16) uint8_t ap_boot_stacks[CPU_BOOT_MAX_COUNT][CPU_BOOT_STACK_SIZE];
static uint8_t cpu_current_ok[CPU_BOOT_MAX_COUNT];

#if defined(PLATFORM_PC_X86_64)
static uint64_t kernel_cpu_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->lapic_id : 0u;
}

static uint64_t kernel_cpu_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t kernel_cpu_boot_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? (uint64_t)response->bsp_lapic_id : 0u;
}

static bool kernel_cpu_boot_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_AARCH64)
static uint64_t kernel_cpu_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? info->mpidr : 0u;
}

static uint64_t kernel_cpu_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t kernel_cpu_boot_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? response->bsp_mpidr : 0u;
}

static bool kernel_cpu_boot_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_RISCV64)
static uint64_t kernel_cpu_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? info->hartid : 0u;
}

static uint64_t kernel_cpu_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? info->processor_id : 0u;
}

static uint64_t kernel_cpu_boot_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? response->bsp_hartid : 0u;
}

static bool kernel_cpu_boot_mp_supported(void) {
	return true;
}
#else
static uint64_t kernel_cpu_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	(void)info;
	return 0u;
}

static uint64_t kernel_cpu_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	(void)info;
	return 0u;
}

static uint64_t kernel_cpu_boot_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	(void)response;
	return 0u;
}

static bool kernel_cpu_boot_mp_supported(void) {
	return false;
}
#endif

#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64)
static void kernel_cpu_mp_entry(struct LIMINE_MP(info) * info) {
	struct cpu* cpu;

	if (!info) {
		for (;;) {
			hal_cpu_park();
		}
	}

	cpu                 = (struct cpu*)(uintptr_t)info->extra_argument;
	cpu->limine_mp_info = info;
	kernel_cpu_boot_bind_current(cpu);
	(void)cpu_set_state(cpu, CPU_STATE_STARTING);
	if (!hal_interrupts_init_local(cpu)) {
		for (;;) {
			hal_cpu_park();
		}
	}
	(void)cpu_set_state(cpu, CPU_STATE_ONLINE);
	for (;;) {
		hal_cpu_park();
	}
}
#endif

bool kernel_cpu_boot_init(uintptr_t boot_stack_base, uintptr_t boot_stack_top) {
	size_t cpu_count = 1u;
	size_t bsp_index = 0u;

	for (size_t i = 0; i < CPU_BOOT_MAX_COUNT; i++) {
		cpu_current_ok[i] = 0u;
	}

	boot_cpu_init[0] = (struct cpu_init_info){
		.index           = 0u,
		.processor_id    = 0u,
		.arch_id         = hal_cpu_boot_arch_id(),
		.role            = CPU_ROLE_BSP,
		.boot_stack_base = boot_stack_base,
		.boot_stack_top  = boot_stack_top,
		.limine_mp_info  = NULL,
	};

	if (kernel_cpu_boot_mp_supported() && mp_req.response != NULL && mp_req.response->cpus != NULL &&
	    mp_req.response->cpu_count > 0u && mp_req.response->cpu_count <= CPU_BOOT_MAX_COUNT) {
		const uint64_t bsp_arch_id = kernel_cpu_boot_bsp_arch_id(mp_req.response);

		cpu_count = (size_t)mp_req.response->cpu_count;
		for (size_t i = 0; i < cpu_count; i++) {
			struct LIMINE_MP(info)* info   = mp_req.response->cpus[i];
			const uint64_t      arch_id    = kernel_cpu_boot_mp_info_arch_id(info);
			const enum cpu_role role       = arch_id == bsp_arch_id ? CPU_ROLE_BSP : CPU_ROLE_AP;
			const uintptr_t     stack_base = role == CPU_ROLE_BSP ? boot_stack_base : (uintptr_t)ap_boot_stacks[i];
			const uintptr_t     stack_top  = role == CPU_ROLE_BSP ? boot_stack_top : stack_base + CPU_BOOT_STACK_SIZE;

			boot_cpu_init[i] = (struct cpu_init_info){
				.index           = i,
				.processor_id    = kernel_cpu_boot_mp_info_processor_id(info),
				.arch_id         = arch_id,
				.role            = role,
				.boot_stack_base = stack_base,
				.boot_stack_top  = stack_top,
				.limine_mp_info  = info,
			};
			if (role == CPU_ROLE_BSP) bsp_index = i;
		}
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
	if (!kernel_cpu_boot_mp_supported()) return true;

	for (size_t i = 0; i < cpu_count(); i++) {
		struct cpu* cpu = cpu_by_index(i);
#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64)
		struct LIMINE_MP(info) * info;
#endif

		if (cpu == NULL || cpu->role != CPU_ROLE_AP) continue;
#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64)
		info = (struct LIMINE_MP(info)*)cpu->limine_mp_info;
		if (!info) continue;

		(void)cpu_set_state(cpu, CPU_STATE_STARTING);
		__atomic_store_n(&info->extra_argument, (uint64_t)(uintptr_t)cpu, __ATOMIC_SEQ_CST);
		__atomic_store_n(&info->goto_address, kernel_cpu_mp_entry, __ATOMIC_SEQ_CST);

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
#endif
	}

	return true;
}

bool kernel_cpu_boot_current_pointer_ok(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= CPU_BOOT_MAX_COUNT) return false;
	return __atomic_load_n(&cpu_current_ok[cpu->index], __ATOMIC_ACQUIRE) != 0u;
}
