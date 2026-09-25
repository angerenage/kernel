#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return the number of discovered fixed-source domains. */
size_t loongarch64_fixed_source_domain_count(void);

/* Return a fixed-source domain by enumeration index. */
bool loongarch64_fixed_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain);

/* Resolve a firmware-visible controller address and local ID without claiming the source. */
bool loongarch64_fixed_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                      struct hal_interrupt_source* out_source);

/* Return whether ownership of a fixed source is available to userspace. */
bool loongarch64_fixed_source_claimable(const struct hal_interrupt_source* source);

/* Return whether a fixed-source configuration is supported by the platform. */
bool loongarch64_fixed_source_configuration_supported(const struct hal_interrupt_source* source,
                                                      enum hal_interrupt_trigger         trigger,
                                                      enum hal_interrupt_polarity        polarity);

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
