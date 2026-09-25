#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

/* Discover the local and I/O APICs needed before secondary CPU startup. */
bool apic_prepare_ipi(void);

/* Initialize the local APIC interface on the current processor. */
bool apic_init_local(void);

/* Return whether the APIC can deliver inter-processor interrupts. */
bool apic_ipi_ready(void);

/* Discover which ISA sources have an I/O APIC route without installing one. */
bool apic_probe_isa_irqs(void);

/* Return whether an ISA source has a firmware-described I/O APIC route. */
bool apic_isa_irq_available(unsigned irq);

/* Resolve one firmware-visible I/O APIC and redirection index into a HAL source. */
bool apic_resolve_ioapic_source(uint64_t controller_address, uint32_t local_source_id,
                                struct hal_interrupt_source* out_source);

/* Return whether a resolved non-ISA I/O APIC source is available. */
bool apic_ioapic_source_available(const struct hal_interrupt_source* source);

/* Return whether a raw I/O APIC source may be claimed without aliasing a canonical ISA source. */
bool apic_ioapic_source_claimable(const struct hal_interrupt_source* source);

/* Program an I/O APIC route for one ISA interrupt source. */
bool apic_route_isa_irq(unsigned irq, unsigned vector, uint32_t target_lapic_id, enum hal_interrupt_trigger trigger,
                        enum hal_interrupt_polarity polarity, uint32_t* out_route, uintptr_t* out_registers);

/* Program one resolved non-ISA I/O APIC source. */
bool apic_route_ioapic_source(const struct hal_interrupt_source* source, unsigned vector, uint32_t target_lapic_id,
                              enum hal_interrupt_trigger trigger, enum hal_interrupt_polarity polarity,
                              uint32_t* out_route, uintptr_t* out_registers);

/* Change the mask state of an I/O APIC ISA route. */
bool apic_set_isa_irq_mask(uintptr_t registers, uint32_t route, bool masked);

/* Signal end-of-interrupt to the local APIC. */
void apic_send_eoi(void);

/* Send a fixed-vector inter-processor interrupt through the local APIC. */
bool apic_send_ipi(uint32_t lapic_id, unsigned vector);

/* Send a non-maskable interrupt through the local APIC. */
bool apic_send_nmi(uint32_t lapic_id);
