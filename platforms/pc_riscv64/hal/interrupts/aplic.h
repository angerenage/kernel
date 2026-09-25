#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* Return the fixed-source domain exposed by the APLIC. */
bool riscv64_aplic_source_domain_at(struct hal_interrupt_source_domain_info* out);

bool riscv64_aplic_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source);

/* Return the delivery identity and target-routing capability of one APLIC source. */
bool riscv64_aplic_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out);

/* Return whether an APLIC source can deliver to a CPU without changing hardware state. */
bool riscv64_aplic_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* Configure an APLIC interrupt source and leave it masked. */
bool riscv64_aplic_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery);

/* Mask an initialized APLIC interrupt source. */
bool riscv64_aplic_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized APLIC interrupt source. */
bool riscv64_aplic_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release an initialized APLIC interrupt source. */
bool riscv64_aplic_source_deinit(struct hal_interrupt_source_state* state);

/* Return whether a CPU has an APLIC delivery interface. */
bool riscv64_aplic_cpu_has_interface(const struct cpu* cpu);

/* Dispatch one directly delivered APLIC interrupt. */
bool riscv64_aplic_handle_external_irq(void);

/* Dispatch an APLIC interrupt delivered through an IMSIC identity. */
bool riscv64_aplic_handle_message_id(uint32_t id);
