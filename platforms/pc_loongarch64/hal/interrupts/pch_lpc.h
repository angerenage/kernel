#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct fixed_controller;

/* Map and initialize a PCH low-pin-count interrupt controller. */
bool loongarch64_pch_lpc_probe(struct fixed_controller* controller);

/* Change the mask state of one PCH-LPC source. */
bool loongarch64_pch_lpc_set_masked(struct fixed_controller* controller, uint32_t source, bool masked);

/* Configure the polarity of one PCH-LPC source. */
bool loongarch64_pch_lpc_configure(struct fixed_controller* controller, uint32_t source,
                                   const struct hal_interrupt_delivery* delivery);

/* Mask and acknowledge PCH-LPC sources pending behind a parent cascade. */
bool loongarch64_pch_lpc_handle_cascade(uint32_t parent_source);
