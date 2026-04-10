#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Logical role assigned to a processor in the boot topology, bootstrap processor or application processor. */
enum cpu_role {
	CPU_ROLE_BSP = 0,
	CPU_ROLE_AP,
};

/* Lifecycle state tracked by the kernel for each discovered CPU. */
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

struct thread;

/*
 * Per-CPU state shared across scheduler, interrupt, and lock-debug code.
 *
 * Important fields:
 * - processor_id: boot protocol CPU index or firmware-facing identifier
 * - arch_id: hardware-local identifier used by the active architecture backend
 * - current_thread: thread currently considered running on this CPU
 * - interrupts_ready: becomes true after local interrupt state has been installed
 * - irq_disable_depth / exception_depth: nesting counters for irq-save and trap handling
 * - boot_stack_*: stack bounds used during CPU bring-up before normal scheduling exists
 */
struct cpu {
	size_t         index;
	uint64_t       processor_id;
	uint64_t       arch_id;
	enum cpu_role  role;
	enum cpu_state state;
	struct thread* current_thread;
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

/* Global CPU inventory discovered during boot. */
struct cpu_topology {
	struct cpu* cpus;
	size_t      cpu_count;
	size_t      online_count;
	size_t      bsp_index;
};

/* Boot-time description used to seed the cpu_topology. */
struct cpu_init_info {
	size_t        index;
	uint64_t      processor_id;
	uint64_t      arch_id;
	enum cpu_role role;
	uintptr_t     boot_stack_base;
	uintptr_t     boot_stack_top;
};

/* Install a full CPU topology discovered from the boot environment. Resets online_count to zero. */
bool cpu_topology_init(const struct cpu_init_info* init_info, size_t cpu_count, size_t bsp_index);

/* Bootstrap-only fallback that creates a topology containing just the currently executing bootstrap processor. */
bool cpu_topology_init_bootstrap(uintptr_t boot_stack_base, uintptr_t boot_stack_top);

/* Return the global topology record. */
struct cpu_topology* cpu_topology_get(void);

/* Return the struct cpu bound to the running hardware thread through hal_cpu_local_bind(). */
struct cpu* cpu_current(void);

/* Return the bootstrap processor entry from the topology, or NULL if topology init has not completed. */
struct cpu* cpu_bsp(void);

/* Return a CPU by its stable topology index. */
struct cpu* cpu_by_index(size_t index);

/* Bind a struct cpu to the current hardware thread so cpu_current() and lock debug state work. */
void cpu_bind_current(struct cpu* cpu);

/* Transition a CPU between lifecycle states and keep the online CPU count consistent. */
bool cpu_set_state(struct cpu* cpu, enum cpu_state state);

/* Mark whether per-CPU interrupt setup has completed; debug lock checks use this to enforce irqsafe rules. */
void cpu_interrupts_set_ready(struct cpu* cpu, bool ready);

/* Convenience accessor for cpu_current()->index, falling back to 0 when no CPU is bound yet. */
size_t cpu_index(void);

/* Convenience accessor for cpu_current()->arch_id. */
uint64_t cpu_arch_id(void);

/* Return true when the current CPU is the topology's bootstrap processor entry. */
bool cpu_is_bsp(void);

/* Return the number of CPUs present in the topology. */
size_t cpu_count(void);

/* Return the number of CPUs whose state is currently ONLINE. */
size_t cpu_online_count(void);

/* Enter one level of exception nesting for the current CPU. */
void cpu_enter_exception(void);

/* Leave one level of exception nesting for the current CPU. */
void cpu_leave_exception(void);

/* Return whether the current CPU is presently executing inside an exception/trap context. */
bool cpu_irq_in_exception(void);

/* Read a CPU state using acquire semantics. NULL is treated as HALTED. */
enum cpu_state cpu_state_get(const struct cpu* cpu);
