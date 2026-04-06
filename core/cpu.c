#include <core/cpu.h>
#include <hal/cpu.h>
#include <stddef.h>

static struct cpu          bootstrap_cpu;
static struct cpu_topology topology;

bool cpu_topology_init_bootstrap(uintptr_t boot_stack_base, uintptr_t boot_stack_top) {
	bootstrap_cpu = (struct cpu){
		.index             = 0u,
		.processor_id      = 0u,
		.arch_id           = hal_cpu_boot_arch_id(),
		.role              = CPU_ROLE_BSP,
		.state             = CPU_STATE_PRESENT,
		.interrupts_ready  = false,
		.irq_disable_depth = 0u,
		.exception_depth   = 0u,
		.boot_stack_base   = boot_stack_base,
		.boot_stack_top    = boot_stack_top,
		.limine_mp_info    = NULL,
	};

	topology = (struct cpu_topology){
		.cpus         = &bootstrap_cpu,
		.cpu_count    = 1u,
		.online_count = 0u,
		.bsp_index    = 0u,
	};
	return true;
}

struct cpu_topology* cpu_topology_get(void) {
	return &topology;
}

struct cpu* cpu_current(void) {
	return (struct cpu*)hal_cpu_local_current();
}

struct cpu* cpu_bsp(void) {
	if (topology.cpus == NULL || topology.bsp_index >= topology.cpu_count) return NULL;
	return &topology.cpus[topology.bsp_index];
}

struct cpu* cpu_by_index(size_t index) {
	if (topology.cpus == NULL || index >= topology.cpu_count) return NULL;
	return &topology.cpus[index];
}

void cpu_bind_current(struct cpu* cpu) {
	if (!cpu) return;
	hal_cpu_local_bind(cpu);
}

bool cpu_set_state(struct cpu* cpu, enum cpu_state state) {
	if (!cpu) return false;

	if (cpu->state != CPU_STATE_ONLINE && state == CPU_STATE_ONLINE) topology.online_count++;
	if (cpu->state == CPU_STATE_ONLINE && state != CPU_STATE_ONLINE && topology.online_count != 0u)
		topology.online_count--;
	cpu->state = state;
	return true;
}

void cpu_interrupts_set_ready(struct cpu* cpu, bool ready) {
	if (!cpu) return;
	cpu->interrupts_ready = ready;
}

size_t cpu_index(void) {
	struct cpu* cpu = cpu_current();
	return cpu ? cpu->index : 0u;
}

uint64_t cpu_arch_id(void) {
	struct cpu* cpu = cpu_current();
	return cpu ? cpu->arch_id : 0u;
}

bool cpu_is_bsp(void) {
	struct cpu* cpu = cpu_current();
	return cpu != NULL && cpu->role == CPU_ROLE_BSP;
}

size_t cpu_count(void) {
	return topology.cpu_count;
}

size_t cpu_online_count(void) {
	return topology.online_count;
}

void cpu_enter_exception(void) {
	struct cpu* cpu = cpu_current();
	if (!cpu) return;
	cpu->exception_depth++;
}

void cpu_leave_exception(void) {
	struct cpu* cpu = cpu_current();
	if (!cpu || cpu->exception_depth == 0u) return;
	cpu->exception_depth--;
}

bool cpu_irq_in_exception(void) {
	struct cpu* cpu = cpu_current();
	return cpu != NULL && cpu->exception_depth != 0u;
}
