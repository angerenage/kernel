#pragma once

#include <hal/clock.h>
#include <stdbool.h>
#include <stdint.h>

#include "utils.h"

#define X86_IRQ_BASE 32u
#define X86_IRQ_COUNT 16u
#define X86_PIC1_CMD 0x20u
#define X86_PIC1_DATA 0x21u
#define X86_PIC2_CMD 0xa0u
#define X86_PIC2_DATA 0xa1u
#define X86_PIC_EOI 0x20u
#define X86_ICW1_INIT 0x10u
#define X86_ICW1_ICW4 0x01u
#define X86_ICW4_8086 0x01u
#define X86_PIT_CHANNEL0 0x40u
#define X86_PIT_COMMAND 0x43u
#define X86_PIT_INPUT_HZ 1193182u
#define X86_PAGE_SIZE 0x1000u
#define X86_IA32_APIC_BASE_MSR 0x1bu
#define X86_IA32_APIC_BASE_ENABLE (1ull << 11)
#define X86_IA32_APIC_BASE_ADDR_MASK 0xfffff000ull
#define X86_LAPIC_ID_REG 0x20u
#define X86_LAPIC_TPR_REG 0x80u
#define X86_LAPIC_EOI_REG 0x0b0u
#define X86_LAPIC_SVR_REG 0x0f0u
#define X86_LAPIC_SVR_ENABLE 0x100u
#define X86_LAPIC_SPURIOUS_VECTOR 0xffu
#define X86_IOAPIC_REGSEL 0x00u
#define X86_IOAPIC_WINDOW 0x10u
#define X86_IOAPIC_VERSION_REG 0x01u
#define X86_IOAPIC_REDIR_BASE 0x10u
#define X86_IOAPIC_REDIR_POLARITY_LOW (1ull << 13)
#define X86_IOAPIC_REDIR_TRIGGER_LEVEL (1ull << 15)
#define X86_ACPI_MADT_TYPE_IO_APIC 1u
#define X86_ACPI_MADT_TYPE_INTERRUPT_SOURCE_OVERRIDE 2u
#define X86_ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE 5u
#define X86_ACPI_MADT_POLARITY_MASK 0x0003u
#define X86_ACPI_MADT_POLARITY_ACTIVE_LOW 0x0003u
#define X86_ACPI_MADT_TRIGGER_MASK 0x000cu
#define X86_ACPI_MADT_TRIGGER_LEVEL 0x000cu

struct interrupt_frame {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
};

void pic_init(void);
void pic_unmask_irq(unsigned irq);
void pic_send_eoi(unsigned vector);
bool pit_init(uint32_t frequency_hz, uint32_t* actual_frequency_hz);

bool apic_route_isa_irq(unsigned irq, unsigned vector);
bool apic_is_active(void);
void apic_send_eoi(void);

void interrupts_init_traps(void);
void interrupts_enable(void);
void interrupts_disable(void);
bool clock_handle_irq(unsigned vector);
