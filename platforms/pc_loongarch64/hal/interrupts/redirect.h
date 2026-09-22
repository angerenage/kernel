#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* Return whether REDIRECTINT can deliver a remapped message to a processor. */
bool loongarch64_redirect_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                           const struct cpu* target);

/* Allocate and program one REDIRECTINT entry for a core-reserved AVEC vector. */
bool loongarch64_redirect_init(struct hal_interrupt_message_state*         state,
                               const struct hal_interrupt_message_request* request,
                               struct hal_interrupt_message*               out_message);

/* Invalidate and release one initialized REDIRECTINT entry. */
bool loongarch64_redirect_deinit(struct hal_interrupt_message_state* state);
