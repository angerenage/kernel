#pragma once

#include <base/interrupt.h>
#include <base/syscall.h>

/* Resolve a firmware-described fixed-source identity into an opaque token. */
syscall_status_t interrupts_resolve_source(cap_id_t interrupts_cap, uint64_t controller_register_address,
                                           uint32_t local_source_id, interrupt_source_t* out_source);

/* Resolve a firmware/device message producer into an opaque allocation context. */
syscall_status_t interrupts_resolve_message_context(cap_id_t interrupts_cap, uint64_t controller_register_address,
                                                    uint32_t producer_id, interrupt_message_context_t* out_context);

/* Claim and configure one fixed interrupt source and return its Interrupt capability. */
syscall_status_t interrupts_claim_source(cap_id_t interrupts_cap, interrupt_source_t source,
                                         enum interrupt_trigger trigger, enum interrupt_polarity polarity,
                                         cap_id_t* out_interrupt_cap);

/* Allocate one message interrupt and return its capability and device-programmable message. */
syscall_status_t interrupts_allocate_message(cap_id_t interrupts_cap, interrupt_message_context_t context,
                                             cap_id_t* out_interrupt_cap, struct interrupt_message* out_message);

/* Read the public state of an Interrupt capability. */
syscall_status_t interrupt_info(cap_id_t interrupt_cap, struct interrupt_info* out_info);

/* Bind an Interrupt to a Signal capability. */
syscall_status_t interrupt_bind(cap_id_t interrupt_cap, cap_id_t signal_cap);

/* Detach the Signal while retaining ownership of the Interrupt. */
syscall_status_t interrupt_unbind(cap_id_t interrupt_cap);

/* Destroy an Interrupt and invalidate every capability grant that references it. */
syscall_status_t interrupt_destroy(cap_id_t interrupt_cap);
