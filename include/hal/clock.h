#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The kernel currently exposes a single periodic clock source. */
typedef void (*hal_clock_handler_t)(void* ctx);

/* Reset backend timer state without starting periodic delivery yet. Safe to call more than once. */
void hal_clock_init(void);

/* Return the frequency of the architecture's continuous monotonic counter, or 0 when unavailable. */
uint64_t hal_clock_monotonic_frequency(void);

/* Read the architecture's continuous monotonic counter. */
bool hal_clock_monotonic_counter(uint64_t* out_counter);

/* Start or restart periodic ticks. Replaces any previous handler and reports the actual programmed frequency. */
bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx);

/* Return the currently programmed tick rate, or 0 when the periodic source is stopped. */
uint32_t hal_clock_frequency(void);

/* Stop periodic delivery and mask any architecture-specific timer interrupt source. */
void hal_clock_stop(void);
