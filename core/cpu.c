#include <core/cpu.h>
#include <core/lock.h>
#include <core/spinlock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>

#define CPU_MAX_COUNT 64u

static struct cpu          cpu_storage[CPU_MAX_COUNT];
static struct cpu_topology topology;
static struct spinlock     cpu_topology_lock =
	SPINLOCK_INIT_CLASS("cpu_topology_lock", SPINLOCK_ORDER_CPU_TOPOLOGY, SPINLOCK_FLAG_IRQSAVE);

bool cpu_topology_init(const struct cpu_init_info* init_info, size_t cpu_count, size_t bsp_index) {
	struct irq_state state;

	if (init_info == NULL || cpu_count == 0u || cpu_count > CPU_MAX_COUNT || bsp_index >= cpu_count) return false;

	state = spinlock_lock_irqsave(&cpu_topology_lock);
	for (size_t i = 0; i < cpu_count; i++) {
		enum cpu_role role = i == bsp_index ? CPU_ROLE_BSP : init_info[i].role;

		if (role == CPU_ROLE_BSP && i != bsp_index) role = CPU_ROLE_AP;
		cpu_storage[i] = (struct cpu){
			.index                = init_info[i].index,
			.processor_id         = init_info[i].processor_id,
			.arch_id              = init_info[i].arch_id,
			.role                 = role,
			.state                = CPU_STATE_PRESENT,
			.current_thread       = NULL,
			.reschedule_requested = false,
			.interrupts_ready     = false,
			.irq_disable_depth    = 0u,
			.exception_depth      = 0u,
			.boot_stack_base      = init_info[i].boot_stack_base,
			.boot_stack_top       = init_info[i].boot_stack_top,
		};
	}

	topology = (struct cpu_topology){
		.cpus         = cpu_storage,
		.cpu_count    = cpu_count,
		.online_count = 0u,
		.bsp_index    = bsp_index,
	};
	spinlock_unlock_irqrestore(&cpu_topology_lock, state);
	return true;
}

bool cpu_topology_init_bootstrap(uintptr_t boot_stack_base, uintptr_t boot_stack_top) {
	const struct cpu_init_info init_info = {
		.index           = 0u,
		.processor_id    = 0u,
		.arch_id         = hal_cpu_boot_arch_id(),
		.role            = CPU_ROLE_BSP,
		.boot_stack_base = boot_stack_base,
		.boot_stack_top  = boot_stack_top,
	};

	return cpu_topology_init(&init_info, 1u, 0u);
}

struct cpu_topology* cpu_topology_get(void) {
	return &topology;
}

struct cpu* cpu_current(void) {
	return (struct cpu*)hal_cpu_local_current();
}

struct cpu* cpu_bsp(void) {
	struct irq_state state;
	struct cpu*      cpu;

	state = spinlock_lock_irqsave(&cpu_topology_lock);
	if (topology.cpus == NULL || topology.bsp_index >= topology.cpu_count) {
		spinlock_unlock_irqrestore(&cpu_topology_lock, state);
		return NULL;
	}
	cpu = &topology.cpus[topology.bsp_index];
	spinlock_unlock_irqrestore(&cpu_topology_lock, state);
	return cpu;
}

struct cpu* cpu_by_index(size_t index) {
	struct irq_state state;
	struct cpu*      cpu;

	state = spinlock_lock_irqsave(&cpu_topology_lock);
	if (topology.cpus == NULL || index >= topology.cpu_count) {
		spinlock_unlock_irqrestore(&cpu_topology_lock, state);
		return NULL;
	}
	cpu = &topology.cpus[index];
	spinlock_unlock_irqrestore(&cpu_topology_lock, state);
	return cpu;
}

void cpu_bind_current(struct cpu* cpu) {
	if (!cpu) return;
	hal_cpu_local_bind(cpu);
}

bool cpu_set_state(struct cpu* cpu, enum cpu_state state) {
	struct irq_state irq_state;

	if (!cpu) return false;

	irq_state = spinlock_lock_irqsave(&cpu_topology_lock);
	if (cpu->state != CPU_STATE_ONLINE && state == CPU_STATE_ONLINE) topology.online_count++;
	if (cpu->state == CPU_STATE_ONLINE && state != CPU_STATE_ONLINE && topology.online_count != 0u)
		topology.online_count--;
	__atomic_store_n((unsigned*)&cpu->state, (unsigned)state, __ATOMIC_RELEASE);
	spinlock_unlock_irqrestore(&cpu_topology_lock, irq_state);
	return true;
}

void cpu_interrupts_set_ready(struct cpu* cpu, bool ready) {
	struct irq_state irq_state;

	if (!cpu) return;
	irq_state             = spinlock_lock_irqsave(&cpu_topology_lock);
	cpu->interrupts_ready = ready;
	spinlock_unlock_irqrestore(&cpu_topology_lock, irq_state);
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
	size_t           count;
	struct irq_state state = spinlock_lock_irqsave(&cpu_topology_lock);

	count = topology.cpu_count;
	spinlock_unlock_irqrestore(&cpu_topology_lock, state);
	return count;
}

size_t cpu_online_count(void) {
	size_t           count;
	struct irq_state state = spinlock_lock_irqsave(&cpu_topology_lock);

	count = topology.online_count;
	spinlock_unlock_irqrestore(&cpu_topology_lock, state);
	return count;
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

enum cpu_state cpu_state_get(const struct cpu* cpu) {
	if (!cpu) return CPU_STATE_HALTED;
	return (enum cpu_state)__atomic_load_n((const unsigned*)&cpu->state, __ATOMIC_ACQUIRE);
}
