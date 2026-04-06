#include <hal/paging.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "interrupts_private.h"

struct x86_acpi_rsdp {
	char     signature[8];
	uint8_t  checksum;
	char     oem_id[6];
	uint8_t  revision;
	uint32_t rsdt_address;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct x86_acpi_sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct x86_acpi_madt {
	struct x86_acpi_sdt_header header;
	uint32_t                   lapic_address;
	uint32_t                   flags;
} __attribute__((packed));

struct x86_acpi_madt_entry_header {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

struct x86_acpi_madt_io_apic {
	struct x86_acpi_madt_entry_header header;
	uint8_t                           io_apic_id;
	uint8_t                           reserved;
	uint32_t                          io_apic_address;
	uint32_t                          global_system_interrupt_base;
} __attribute__((packed));

struct x86_acpi_madt_iso {
	struct x86_acpi_madt_entry_header header;
	uint8_t                           bus;
	uint8_t                           source;
	uint32_t                          global_system_interrupt;
	uint16_t                          flags;
} __attribute__((packed));

struct x86_acpi_madt_lapic_addr_override {
	struct x86_acpi_madt_entry_header header;
	uint16_t                          reserved;
	uint64_t                          lapic_address;
} __attribute__((packed));

static bool              apic_active;
static volatile uint8_t* lapic_mmio;
static volatile uint8_t* ioapic_mmio;
static bool              apic_irq_route_valid[X86_IRQ_COUNT];
static uint32_t          apic_irq_route_index[X86_IRQ_COUNT];

static uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	return phys + (uintptr_t)hhdm_req.response->offset;
}

static bool map_mmio_page(uintptr_t phys) {
	uintptr_t virt = hhdm_phys_to_virt(phys & ~(uintptr_t)(X86_PAGE_SIZE - 1u));

	uintptr_t existing_phys = 0;
	if (hal_paging_query(virt, &existing_phys, NULL)) return true;

	return hal_paging_map(
		virt, phys & ~(uintptr_t)(X86_PAGE_SIZE - 1u), HAL_PAGE_WRITE | HAL_PAGE_GLOBAL | HAL_PAGE_NO_CACHE);
}

static bool acpi_signature_equals(const char* actual, const char* expected) {
	for (size_t i = 0; i < 4u; i++) {
		if (actual[i] != expected[i]) return false;
	}

	return true;
}

static bool acpi_checksum_valid(const void* table, size_t length) {
	const uint8_t* bytes = (const uint8_t*)table;
	uint8_t        sum   = 0u;

	for (size_t i = 0; i < length; i++) {
		sum = (uint8_t)(sum + bytes[i]);
	}

	return sum == 0u;
}

static const struct x86_acpi_sdt_header* acpi_find_table(const char signature[4]) {
	if (!rsdp_req.response || !rsdp_req.response->address || !hhdm_req.response) return NULL;

	const struct x86_acpi_rsdp* rsdp        = (const struct x86_acpi_rsdp*)(uintptr_t)rsdp_req.response->address;
	size_t                      rsdp_length = rsdp->revision >= 2u ? (size_t)rsdp->length : 20u;

	if (!acpi_checksum_valid(rsdp, rsdp_length)) return NULL;

	if (rsdp->revision >= 2u && rsdp->xsdt_address != 0u) {
		const struct x86_acpi_sdt_header* xsdt =
			(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)rsdp->xsdt_address);
		if (!acpi_checksum_valid(xsdt, xsdt->length)) return NULL;

		size_t          entry_count = (xsdt->length - sizeof(*xsdt)) / sizeof(uint64_t);
		const uint64_t* entries     = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));

		for (size_t i = 0; i < entry_count; i++) {
			const struct x86_acpi_sdt_header* table =
				(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)entries[i]);
			if (acpi_signature_equals(table->signature, signature) && acpi_checksum_valid(table, table->length)) {
				return table;
			}
		}

		return NULL;
	}

	if (rsdp->rsdt_address == 0u) return NULL;

	const struct x86_acpi_sdt_header* rsdt =
		(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)rsdp->rsdt_address);
	if (!acpi_checksum_valid(rsdt, rsdt->length)) return NULL;

	size_t          entry_count = (rsdt->length - sizeof(*rsdt)) / sizeof(uint32_t);
	const uint32_t* entries     = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));

	for (size_t i = 0; i < entry_count; i++) {
		const struct x86_acpi_sdt_header* table =
			(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)entries[i]);
		if (acpi_signature_equals(table->signature, signature) && acpi_checksum_valid(table, table->length)) {
			return table;
		}
	}

	return NULL;
}

static uint32_t lapic_read(uint32_t reg) {
	return *(volatile uint32_t*)(lapic_mmio + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
	*(volatile uint32_t*)(lapic_mmio + reg) = value;
	(void)lapic_read(reg);
}

static uint32_t ioapic_read(uint8_t reg) {
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_REGSEL) = reg;
	return *(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_WINDOW);
}

static void ioapic_write(uint8_t reg, uint32_t value) {
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_REGSEL) = reg;
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_WINDOW) = value;
}

static bool lapic_init(uintptr_t lapic_phys) {
	uint64_t apic_base = read_msr(X86_IA32_APIC_BASE_MSR);

	if ((apic_base & X86_IA32_APIC_BASE_ENABLE) == 0u) {
		apic_base |= X86_IA32_APIC_BASE_ENABLE;
		write_msr(X86_IA32_APIC_BASE_MSR, apic_base);
	}

	if (lapic_phys == 0u) {
		lapic_phys = (uintptr_t)(apic_base & X86_IA32_APIC_BASE_ADDR_MASK);
	}
	if (lapic_phys == 0u || !hhdm_req.response) return false;
	if (!map_mmio_page(lapic_phys)) return false;

	lapic_mmio = (volatile uint8_t*)hhdm_phys_to_virt(lapic_phys);
	lapic_write(X86_LAPIC_TPR_REG, 0u);
	lapic_write(X86_LAPIC_SVR_REG, X86_LAPIC_SVR_ENABLE | X86_LAPIC_SPURIOUS_VECTOR);
	return true;
}

bool apic_route_isa_irq(unsigned irq, unsigned vector) {
	const struct x86_acpi_sdt_header* madt_header = acpi_find_table("APIC");
	if (!madt_header) return false;

	const struct x86_acpi_madt* madt            = (const struct x86_acpi_madt*)madt_header;
	uintptr_t                   lapic_phys      = (uintptr_t)madt->lapic_address;
	uintptr_t                   ioapic_phys     = 0u;
	uint32_t                    ioapic_gsi_base = 0u;
	uint32_t                    routed_gsi      = irq;
	uint16_t                    routed_flags    = 0u;
	const uint8_t*              entry           = (const uint8_t*)madt + sizeof(*madt);
	const uint8_t*              end             = (const uint8_t*)madt + madt->header.length;

	while (entry + sizeof(struct x86_acpi_madt_entry_header) <= end) {
		const struct x86_acpi_madt_entry_header* header = (const struct x86_acpi_madt_entry_header*)entry;
		if (header->length < sizeof(*header) || entry + header->length > end) return false;

		switch (header->type) {
		case X86_ACPI_MADT_TYPE_IO_APIC: {
			const struct x86_acpi_madt_io_apic* io_apic = (const struct x86_acpi_madt_io_apic*)entry;
			if (ioapic_phys == 0u) {
				ioapic_phys     = (uintptr_t)io_apic->io_apic_address;
				ioapic_gsi_base = io_apic->global_system_interrupt_base;
			}
			break;
		}
		case X86_ACPI_MADT_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
			const struct x86_acpi_madt_iso* iso = (const struct x86_acpi_madt_iso*)entry;
			if (iso->bus == 0u && iso->source == irq) {
				routed_gsi   = iso->global_system_interrupt;
				routed_flags = iso->flags;
			}
			break;
		}
		case X86_ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
			const struct x86_acpi_madt_lapic_addr_override* override =
				(const struct x86_acpi_madt_lapic_addr_override*)entry;
			lapic_phys = (uintptr_t) override->lapic_address;
			break;
		}
		default:
			break;
		}

		entry += header->length;
	}

	if (ioapic_phys == 0u || !lapic_init(lapic_phys)) return false;
	if (!map_mmio_page(ioapic_phys)) return false;

	ioapic_mmio = (volatile uint8_t*)hhdm_phys_to_virt(ioapic_phys);

	uint32_t redir_count = ((ioapic_read(X86_IOAPIC_VERSION_REG) >> 16) & 0xffu) + 1u;
	if (routed_gsi < ioapic_gsi_base || routed_gsi >= ioapic_gsi_base + redir_count) return false;

	uint32_t ioapic_index = routed_gsi - ioapic_gsi_base;
	uint64_t redir        = (uint64_t)vector;

	if ((routed_flags & X86_ACPI_MADT_POLARITY_MASK) == X86_ACPI_MADT_POLARITY_ACTIVE_LOW) {
		redir |= X86_IOAPIC_REDIR_POLARITY_LOW;
	}
	if ((routed_flags & X86_ACPI_MADT_TRIGGER_MASK) == X86_ACPI_MADT_TRIGGER_LEVEL) {
		redir |= X86_IOAPIC_REDIR_TRIGGER_LEVEL;
	}

	uint32_t lapic_id = lapic_read(X86_LAPIC_ID_REG) >> 24;
	ioapic_write((uint8_t)(X86_IOAPIC_REDIR_BASE + ioapic_index * 2u + 1u), lapic_id << 24);
	ioapic_write((uint8_t)(X86_IOAPIC_REDIR_BASE + ioapic_index * 2u), (uint32_t)redir);

	if (irq < X86_IRQ_COUNT) {
		apic_irq_route_valid[irq] = true;
		apic_irq_route_index[irq] = ioapic_index;
	}

	apic_active = true;
	printf("kernel: x86_64 ioapic routed irq%u to vector %u (gsi=%u, lapic=%u)\n", irq, vector, routed_gsi, lapic_id);
	return true;
}

bool apic_set_isa_irq_mask(unsigned irq, bool masked) {
	uint32_t ioapic_index;
	uint8_t  low_reg;
	uint32_t low_value;

	if (irq >= X86_IRQ_COUNT || !apic_active || !apic_irq_route_valid[irq]) return false;

	ioapic_index = apic_irq_route_index[irq];
	low_reg      = (uint8_t)(X86_IOAPIC_REDIR_BASE + ioapic_index * 2u);
	low_value    = ioapic_read(low_reg);
	if (masked) {
		low_value |= (uint32_t)X86_IOAPIC_REDIR_MASK;
	}
	else {
		low_value &= ~(uint32_t)X86_IOAPIC_REDIR_MASK;
	}
	ioapic_write(low_reg, low_value);
	return true;
}

bool apic_is_active(void) {
	return apic_active;
}

void apic_send_eoi(void) {
	if (!apic_active) return;
	lapic_write(X86_LAPIC_EOI_REG, 0u);
}
