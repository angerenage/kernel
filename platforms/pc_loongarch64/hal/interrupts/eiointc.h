#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* Initialize the EIOINTC state local to one processor. */
bool loongarch64_eiointc_init_local(const struct cpu* cpu);

/* Route one EIOINTC vector to a target processor. */
void loongarch64_eiointc_route(uint32_t vector, const struct cpu* cpu);

/* Enable or disable delivery of one EIOINTC vector. */
void loongarch64_eiointc_set_enabled(uint32_t vector, bool enabled);

/* Acknowledge and dispatch pending EIOINTC vectors. */
bool loongarch64_eiointc_handle(void);
