#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum cpu_role {
	CPU_ROLE_BSP = 0,
	CPU_ROLE_AP,
};

enum cpu_state {
	CPU_STATE_PRESENT = 0,
	CPU_STATE_STARTING,
	CPU_STATE_ONLINE,
	CPU_STATE_PARKED,
	CPU_STATE_HALTED,
};

#if CORE_LOCK_DEBUG
#define CPU_DEBUG_LOCK_DEPTH 8u
#endif

struct cpu {
	size_t         index;
	uint64_t       processor_id;
	uint64_t       arch_id;
	enum cpu_role  role;
	enum cpu_state state;
	bool           interrupts_ready;
	uint32_t       irq_disable_depth;
	uint32_t       exception_depth;
	uintptr_t      boot_stack_base;
	uintptr_t      boot_stack_top;
#if CORE_LOCK_DEBUG
	uint32_t lock_depth;
	uint32_t lock_order_stack[CPU_DEBUG_LOCK_DEPTH];
#endif
};

struct cpu_topology {
	struct cpu* cpus;
	size_t      cpu_count;
	size_t      online_count;
	size_t      bsp_index;
};

struct cpu_init_info {
	size_t        index;
	uint64_t      processor_id;
	uint64_t      arch_id;
	enum cpu_role role;
	uintptr_t     boot_stack_base;
	uintptr_t     boot_stack_top;
};

bool cpu_topology_init(const struct cpu_init_info* init_info, size_t cpu_count, size_t bsp_index);
bool cpu_topology_init_bootstrap(uintptr_t boot_stack_base, uintptr_t boot_stack_top);

struct cpu_topology* cpu_topology_get(void);
struct cpu*          cpu_current(void);
struct cpu*          cpu_bsp(void);
struct cpu*          cpu_by_index(size_t index);
void                 cpu_bind_current(struct cpu* cpu);
bool                 cpu_set_state(struct cpu* cpu, enum cpu_state state);
void                 cpu_interrupts_set_ready(struct cpu* cpu, bool ready);

size_t   cpu_index(void);
uint64_t cpu_arch_id(void);
bool     cpu_is_bsp(void);
size_t   cpu_count(void);
size_t   cpu_online_count(void);

void           cpu_enter_exception(void);
void           cpu_leave_exception(void);
bool           cpu_irq_in_exception(void);
enum cpu_state cpu_state_get(const struct cpu* cpu);
