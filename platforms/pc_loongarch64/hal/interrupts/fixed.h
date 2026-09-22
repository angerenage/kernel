#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return the number of discovered fixed-source domains. */
size_t loongarch64_fixed_source_domain_count(void);

/* Return a fixed-source domain by enumeration index. */
bool loongarch64_fixed_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain);

/* Return the delivery identity and target-routing capability of one fixed source. */
bool loongarch64_fixed_source_info(const struct hal_interrupt_source* source,
                                   struct hal_interrupt_source_info*  out_info);

/* Return whether a fixed source can deliver to a CPU without changing hardware state. */
bool loongarch64_fixed_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* Configure one fixed interrupt source and leave it masked. */
bool loongarch64_fixed_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                                   const struct hal_interrupt_delivery* delivery);

/* Mask an initialized fixed interrupt source. */
bool loongarch64_fixed_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized fixed interrupt source. */
bool loongarch64_fixed_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release an initialized fixed interrupt source. */
bool loongarch64_fixed_source_deinit(struct hal_interrupt_source_state* state);
