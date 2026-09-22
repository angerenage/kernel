#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Map and initialize the firmware-described HT vector controller. */
bool loongarch64_htvec_probe(void);

/* Enable or disable delivery of one HT vector. */
void loongarch64_htvec_set_enabled(uint32_t vector, bool enabled);

/* Acknowledge and dispatch pending HT vectors. */
bool loongarch64_htvec_handle(void);
