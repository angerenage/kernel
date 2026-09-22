#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

struct cpu;

/* Discover, map, and enable the architecture's global interrupt controller. */
bool aarch64_gic_init_global(void);

/* Prepare the active GIC implementation for secondary CPU startup. */
bool aarch64_gic_prepare_smp(void);

/* Initialize the active GIC CPU interface for one processor. */
bool aarch64_gic_init_local(struct cpu* cpu);

/* Configure one kernel-private local interrupt source and leave it masked. */
bool aarch64_gic_local_source_init(struct hal_interrupt_source_state* state, uint32_t id, const struct cpu* target);

/* Unmask one initialized kernel-private local interrupt source. */
bool aarch64_gic_local_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release one initialized kernel-private local interrupt source. */
bool aarch64_gic_local_source_deinit(struct hal_interrupt_source_state* state);

/* Dispatch one interrupt through the active GIC implementation. */
bool aarch64_gic_handle_irq(const struct exception_frame* frame);
