#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>

struct cpu;

/* Return the fixed-source domain exposed by the PLIC. */
bool riscv64_plic_source_domain_at(struct hal_interrupt_source_domain_info* out_domain);

bool riscv64_plic_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                 struct hal_interrupt_source* out_source);

/* Return the delivery identity and target-routing capability of one PLIC source. */
bool riscv64_plic_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info);

/* Return whether a PLIC source can deliver to a CPU without changing hardware state. */
bool riscv64_plic_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* Configure a PLIC interrupt source and leave it masked. */
bool riscv64_plic_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                              const struct hal_interrupt_delivery* delivery);

/* Mask an initialized PLIC interrupt source. */
bool riscv64_plic_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized PLIC interrupt source. */
bool riscv64_plic_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release an initialized PLIC interrupt source. */
bool riscv64_plic_source_deinit(struct hal_interrupt_source_state* state);

/* Return whether a CPU has at least one enabled PLIC source. */
bool riscv64_plic_cpu_has_enabled_sources(const struct cpu* cpu);

/* Claim, mask, and complete one pending PLIC interrupt. */
bool riscv64_plic_handle_external_irq(void);
