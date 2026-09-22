#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

/* A saved snapshot of the local interrupt-enable state. */
struct irq_state {
	bool enabled;
};

/* Install architecture-wide interrupt state that only needs to be created once during kernel bring-up. */
bool hal_interrupts_init_global(void);

/* Complete interrupt setup for one CPU and mark that CPU as ready to take traps/IRQs. */
bool hal_interrupts_init_local(struct cpu* cpu);

/* An identity supplied by platform firmware or the architecture's local interrupt namespace. */
struct hal_interrupt_source {
	uint32_t domain;
	uint32_t number;
};

/* Select how a fixed interrupt source signals an event. */
enum hal_interrupt_trigger {
	HAL_INTERRUPT_TRIGGER_FIRMWARE = 0,
	HAL_INTERRUPT_TRIGGER_EDGE,
	HAL_INTERRUPT_TRIGGER_LEVEL,
};

/* Select the active electrical level of a fixed interrupt source. */
enum hal_interrupt_polarity {
	HAL_INTERRUPT_POLARITY_FIRMWARE = 0,
	HAL_INTERRUPT_POLARITY_HIGH,
	HAL_INTERRUPT_POLARITY_LOW,
};

/* An interrupt endpoint identity within one architecture-defined delivery namespace. */
struct hal_interrupt_event {
	uint32_t domain;
	uint32_t id;
};

/* A half-open interval of allocatable events in one delivery namespace. */
struct hal_interrupt_delivery_range {
	uint32_t domain;
	uint32_t base;
	uint32_t limit;
};

/* Describe whether a source has a fixed destination or can be routed by the core. */
enum hal_interrupt_target_kind {
	HAL_INTERRUPT_TARGET_FIXED = 0,
	HAL_INTERRUPT_TARGET_ROUTABLE,
};

/* Allocatable events plus a required fixed target, or a core-selectable routable target. */
struct hal_interrupt_source_info {
	struct hal_interrupt_delivery_range delivery;
	enum hal_interrupt_target_kind      target_kind;
	const struct cpu*                   fixed_target;
};

/* A firmware-described namespace containing assignable external interrupt sources. */
struct hal_interrupt_source_domain_info {
	uint32_t domain;
	uint32_t first_source;
	uint32_t source_count;
};

/* Requested delivery configuration for one fixed interrupt source. */
struct hal_interrupt_delivery {
	const struct cpu*           target;
	struct hal_interrupt_event  event;
	enum hal_interrupt_trigger  trigger;
	enum hal_interrupt_polarity polarity;
};

#if defined(PLATFORM_PC_X86_64)
/* x86 fixed-source state selected at compile time. */
struct hal_interrupt_source_state {
	struct hal_interrupt_source source;
	uintptr_t                   ioapic_registers;
	uint32_t                    ioapic_route;
	bool                        uses_pic;
	bool                        initialized;
	bool                        masked;
};
#elif defined(PLATFORM_PC_AARCH64)
/* AArch64 fixed-source state selected at compile time. */
struct hal_interrupt_source_state {
	struct hal_interrupt_source source;
	uintptr_t                   register_base;
	uint32_t                    target_index;
	bool                        initialized;
	bool                        masked;
};
#elif defined(PLATFORM_PC_RISCV64)
/* RISC-V fixed-source state selected at compile time. */
struct hal_interrupt_source_state {
	struct hal_interrupt_source source;
	const struct cpu*           target;
	uint32_t                    route;
	bool                        initialized;
	bool                        masked;
};
#elif defined(PLATFORM_PC_LOONGARCH64)
/* LoongArch fixed-source state selected at compile time. */
struct hal_interrupt_source_state {
	struct hal_interrupt_source source;
	uint32_t                    route;
	bool                        initialized;
	bool                        masked;
};
#else
/* Hosted-test fixed-source state selected when no kernel platform is active. */
struct hal_interrupt_source_state {
	struct hal_interrupt_source source;
	const struct cpu*           target;
	bool                        initialized;
	bool                        masked;
};
#endif

/* Generic source domains exclude kernel-private local timers, IPIs, and per-CPU interrupts. */

/* Return the number of assignable external-source namespaces discovered from platform firmware. */
size_t hal_interrupt_source_domain_count(void);

/* Return the assignable external-source namespace at an enumeration index. */
bool hal_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain);

/* Return the allocatable delivery events and target constraint of one assignable external source. */
bool hal_interrupt_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info);

/* Return whether an assignable external source can deliver to a CPU without changing hardware state. */
bool hal_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* A successful initialization always leaves the fixed source masked. A failed initialization leaves it inactive. */
bool hal_interrupt_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery);

/* Mask an initialized fixed interrupt source. */
bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized fixed interrupt source. */
bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state);

/* Return true after release or if already inactive; a masking failure returns false and preserves the state. */
bool hal_interrupt_source_deinit(struct hal_interrupt_source_state* state);

/* One contiguous event range allocatable through a message-producing mechanism. */
struct hal_interrupt_message_range {
	uint32_t                            domain;
	struct hal_interrupt_delivery_range delivery;
};

/* A producer identity interpreted within one message-domain-specific namespace. */
struct hal_interrupt_message_source {
	uint32_t domain;
	uint32_t id;
};

/* Requested message mechanism, optional producer, target, and core-reserved delivery event. */
struct hal_interrupt_message_request {
	uint32_t                                   domain;
	const struct hal_interrupt_message_source* source;
	const struct cpu*                          target;
	struct hal_interrupt_event                 event;
};

#if defined(PLATFORM_PC_AARCH64)
/* AArch64 message-delivery state selected at compile time. */
struct hal_interrupt_message_state {
	struct hal_interrupt_event event;
	bool                       uses_gicv3;
	bool                       initialized;
};
#elif defined(PLATFORM_PC_RISCV64)
/* RISC-V message-delivery state selected at compile time. */
struct hal_interrupt_message_state {
	const struct cpu*          target;
	struct hal_interrupt_event event;
	bool                       initialized;
};
#elif defined(PLATFORM_PC_LOONGARCH64)
/* LoongArch message-delivery state selected at compile time. */
struct hal_interrupt_message_state {
	struct hal_interrupt_event event;
	bool                       initialized;
};
#else
/* Stateless message-delivery state selected for x86 and hosted tests. */
struct hal_interrupt_message_state {
	bool initialized;
};
#endif

/* Device-programmable address and data for a message-signaled interrupt. */
struct hal_interrupt_message {
	uint64_t address;
	uint32_t data;
};

/* Return the number of firmware-described message-signaled interrupt ranges. */
size_t hal_interrupt_message_range_count(void);

/* Return the message-signaled interrupt range at an enumeration index. */
bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range);

/* Return whether a message mechanism and optional producer can deliver to a CPU without changing hardware state. */
bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target);

/* Program one core-reserved event and produce its device-programmable message. */
bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message);

/* Disable and release an initialized message delivery after the device has stopped emitting it. */
bool hal_interrupt_message_deinit(struct hal_interrupt_message_state* state);

/* Return whether the local CPU currently has maskable interrupts enabled. */
bool irq_enabled(void);

/* Mask interrupts on the current CPU without touching any saved state structure. */
void irq_disable_local(void);

/* Re-enable interrupts on the current CPU. Callers must only do this when the surrounding context allows it. */
void irq_enable_local(void);

/* Snapshot the local interrupt-enable bit, disable interrupts, and update the core nesting counter. */
struct irq_state irq_save_disable(void);

/* Restore the interrupt state previously returned by irq_save_disable(). */
void irq_restore(struct irq_state state);

/* Return true while the current CPU is executing inside a trap/exception handler. */
bool irq_in_exception(void);
