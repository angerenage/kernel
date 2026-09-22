#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>

/* Return the number of message-signaled ranges exposed by PCH-MSI. */
size_t loongarch64_pch_msi_range_count(void);

/* Return the PCH-MSI range at an enumeration index. */
bool loongarch64_pch_msi_range_at(size_t index, struct hal_interrupt_message_range* out_range);

/* Return whether PCH-MSI can deliver to a CPU without changing hardware state. */
bool loongarch64_pch_msi_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                          const struct cpu* target);

/* Initialize one core-reserved PCH-MSI delivery identity. */
bool loongarch64_pch_msi_init(struct hal_interrupt_message_state*         state,
                              const struct hal_interrupt_message_request* request,
                              struct hal_interrupt_message*               out_message);

/* Disable architecture programming for one PCH-MSI delivery identity. */
bool loongarch64_pch_msi_deinit(struct hal_interrupt_message_state* state);
