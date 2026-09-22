#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

/* Discover the platform interrupt-controller topology from firmware. */
bool loongarch64_interrupt_controllers_discover(void);

/* Initialize per-CPU controller state for one processor. */
bool loongarch64_interrupt_controllers_init_local(const struct cpu* cpu);

/* Dispatch an interrupt pending on one or more CPU interrupt inputs. */
bool loongarch64_interrupt_controllers_handle(uint64_t pending);

/* Return whether CPUINTC must enable the AVECINTC message input. */
bool loongarch64_interrupt_controllers_has_avec(void);

/* Return the number of discovered fixed-source domains. */
size_t loongarch64_interrupt_source_domain_count(void);

/* Return a discovered fixed-source domain by enumeration index. */
bool loongarch64_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain);

/* Return the delivery identity and target-routing capability of one fixed source. */
bool loongarch64_interrupt_source_info(const struct hal_interrupt_source* source,
                                       struct hal_interrupt_source_info*  out_info);

/* Return whether a fixed source can deliver to a CPU without changing hardware state. */
bool loongarch64_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target);

/* Configure a fixed interrupt source and leave it masked. */
bool loongarch64_interrupt_source_init(struct hal_interrupt_source_state*   state,
                                       const struct hal_interrupt_source*   source,
                                       const struct hal_interrupt_delivery* delivery);

/* Mask an initialized fixed interrupt source. */
bool loongarch64_interrupt_source_mask(struct hal_interrupt_source_state* state);

/* Unmask an initialized fixed interrupt source. */
bool loongarch64_interrupt_source_unmask(struct hal_interrupt_source_state* state);

/* Mask and release an initialized fixed interrupt source. */
bool loongarch64_interrupt_source_deinit(struct hal_interrupt_source_state* state);

/* Return the number of discovered message-signaled interrupt ranges. */
size_t loongarch64_interrupt_message_range_count(void);

/* Return a message-signaled interrupt range by enumeration index. */
bool loongarch64_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range);

/* Return whether a message mechanism can deliver to a CPU without changing hardware state. */
bool loongarch64_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                                    const struct cpu* target);

/* Initialize one core-reserved message interrupt. */
bool loongarch64_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                        const struct hal_interrupt_message_request* request,
                                        struct hal_interrupt_message*               out_message);

/* Disable architecture programming for one message interrupt. */
bool loongarch64_interrupt_message_deinit(struct hal_interrupt_message_state* state);
