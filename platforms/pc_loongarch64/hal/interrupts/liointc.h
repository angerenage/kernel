#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct fixed_controller;

/* Map and initialize a legacy I/O interrupt controller. */
bool loongarch64_liointc_probe(struct fixed_controller* controller);

/* Return whether an LIOINTC source is reserved for an internal cascade. */
bool loongarch64_liointc_source_reserved(uint32_t source);

/* Change the mask state of one LIOINTC source. */
bool loongarch64_liointc_set_masked(struct fixed_controller* controller, uint32_t source, bool masked);

/* Configure the trigger and polarity of one LIOINTC source. */
bool loongarch64_liointc_configure(struct fixed_controller* controller, uint32_t source,
                                   const struct hal_interrupt_delivery* delivery);

/* Dispatch LIOINTC sources pending on the asserted CPU inputs. */
bool loongarch64_liointc_handle(uint64_t pending_cpu);
