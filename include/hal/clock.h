#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The kernel currently exposes a single periodic clock source. */
typedef void (*hal_clock_handler_t)(void* ctx);

void     hal_clock_init(void);
bool     hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx);
uint32_t hal_clock_frequency(void);
void     hal_clock_stop(void);
