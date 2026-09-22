#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

/* Detect whether the processor and firmware topology support AVECINTC. */
void loongarch64_avec_discover(void);

/* Enable AVECINTC delivery on one processor. */
bool loongarch64_avec_init_local(const struct cpu* cpu);

/* Return whether AVECINTC is the active message-delivery path. */
bool loongarch64_avec_available(void);

/* Return whether REDIRECTINT is present between PCH-MSI and AVECINTC. */
bool loongarch64_avec_uses_redirect(void);

/* Return whether AVECINTC is enabled and ready on a processor. */
bool loongarch64_avec_cpu_supported(const struct cpu* target);

/* Return the single allocatable AVECINTC delivery range. */
bool loongarch64_avec_range(struct hal_interrupt_message_range* out_range);

/* Return whether direct AVECINTC messages can target a processor. */
bool loongarch64_avec_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                       const struct cpu* target);

/* Compose one direct AVECINTC message for a core-reserved vector. */
bool loongarch64_avec_init(struct hal_interrupt_message_state*         state,
                           const struct hal_interrupt_message_request* request,
                           struct hal_interrupt_message*               out_message);

/* Release one direct AVECINTC message delivery. */
bool loongarch64_avec_deinit(struct hal_interrupt_message_state* state);

/* Drain pending AVECINTC vectors from the current processor. */
bool loongarch64_avec_handle(void);
