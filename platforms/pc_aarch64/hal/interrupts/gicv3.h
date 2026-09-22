#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>

#include "frame.h"

struct cpu;

/* Return whether firmware describes a usable GICv3 controller. */
bool aarch64_gicv3_described(void);

/* Return whether global GICv3 initialization has completed. */
bool aarch64_gicv3_ready(void);

/* Discover and initialize the system-wide GICv3 state. */
bool aarch64_gicv3_init_global(void);

/* Initialize the GICv3 CPU interface for one processor. */
bool aarch64_gicv3_init_local(struct cpu* cpu);

/* Return the number of fixed-source domains exposed by GICv3. */
size_t aarch64_gicv3_source_domain_count(void);

/* Return the GICv3 fixed-source domain at an enumeration index. */
bool aarch64_gicv3_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out);

/* Return the delivery identity and target-routing capability of one GICv3 source. */
bool aarch64_gicv3_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out);

/* Return whether a GICv3 source can deliver to a CPU without changing hardware state. */
bool aarch64_gicv3_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* Configure a GICv3 fixed source and leave it masked. */
bool aarch64_gicv3_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery);

/* Configure one kernel-private local GICv3 source and leave it masked. */
bool aarch64_gicv3_local_source_init(struct hal_interrupt_source_state* state, uint32_t id, const struct cpu* target);

/* Mask an initialized GICv3 fixed source. */
bool aarch64_gicv3_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized GICv3 fixed source. */
bool aarch64_gicv3_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release an initialized GICv3 fixed source. */
bool aarch64_gicv3_source_deinit(struct hal_interrupt_source_state* state);

/* Return the number of message-signaled ranges exposed by GICv3. */
size_t aarch64_gicv3_message_range_count(void);

/* Return the GICv3 message-signaled range at an enumeration index. */
bool aarch64_gicv3_message_range_at(size_t index, struct hal_interrupt_message_range* out);

/* Return whether a GICv3 message mechanism can deliver to a CPU without changing hardware state. */
bool aarch64_gicv3_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target);

/* Initialize one core-reserved GICv3 message interrupt. */
bool aarch64_gicv3_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request, struct hal_interrupt_message* out);

/* Disable the architecture programming for a GICv3 message interrupt. */
bool aarch64_gicv3_message_deinit(struct hal_interrupt_message_state* state);

/* Prepare GICv3 redistributors for secondary CPU startup. */
bool aarch64_gicv3_prepare_smp(void);

/* Acknowledge and dispatch one interrupt through GICv3. */
bool aarch64_gicv3_handle_irq(const struct exception_frame* frame);
