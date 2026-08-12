#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts_private.h"

#define X86_ACPI_RSDP_V1_LENGTH 20u
#define X86_ACPI_RSDP_MAX_LENGTH 4096u
#define X86_ACPI_SDT_MAX_LENGTH (16u * 1024u * 1024u)

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
static bool              lapic_ready;
static volatile uint8_t* lapic_mmio;
static volatile uint8_t* ioapic_mmio;
static bool              apic_irq_route_valid[X86_IRQ_COUNT];
static uint32_t          apic_irq_route_index[X86_IRQ_COUNT];

static bool boot_address_space(struct kernel_boot_address_space* out) {
	return kernel_boot_address_space_get(out);
}

static bool boot_address_space_available(void) {
	struct kernel_boot_address_space address_space;

	return boot_address_space(&address_space);
}

static uintptr_t hhdm_phys_to_virt(uintptr_t phys) {
	struct kernel_boot_address_space address_space;

	if (!boot_address_space(&address_space)) return 0u;
	return phys + address_space.direct_map_offset;
}

static bool map_mmio_page(uintptr_t phys) {
	uintptr_t virt = hhdm_phys_to_virt(phys & ~(uintptr_t)(X86_PAGE_SIZE - 1u));

	uintptr_t existing_phys = 0;
	if (hal_paging_query(hal_paging_kernel_space(), virt, &existing_phys, NULL)) return true;

	return hal_paging_map(hal_paging_kernel_space(),
	                      virt,
	                      phys & ~(uintptr_t)(X86_PAGE_SIZE - 1u),
	                      HAL_PAGE_WRITE | HAL_PAGE_GLOBAL | HAL_PAGE_NO_CACHE);
}

static bool acpi_signature_equals(const char* actual, const char* expected) {
	for (size_t i = 0; i < 4u; i++) {
		if (actual[i] != expected[i]) return false;
	}

	return true;
}

static bool acpi_rsdp_signature_valid(const struct x86_acpi_rsdp* rsdp) {
	static const char signature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};

	if (rsdp == NULL) return false;
	for (size_t i = 0u; i < sizeof(signature); i++) {
		if (rsdp->signature[i] != signature[i]) return false;
	}
	return true;
}

static bool acpi_checksum_valid(const void* table, size_t length) {
	const uint8_t* bytes = (const uint8_t*)table;
	uint8_t        sum   = 0u;

	if (table == NULL || length == 0u) return false;
	for (size_t i = 0; i < length; i++) {
		sum = (uint8_t)(sum + bytes[i]);
	}

	return sum == 0u;
}

static bool acpi_rsdp_valid(const struct x86_acpi_rsdp* rsdp) {
	if (!acpi_rsdp_signature_valid(rsdp) || !acpi_checksum_valid(rsdp, X86_ACPI_RSDP_V1_LENGTH)) return false;
	if (rsdp->revision < 2u) return true;
	if (rsdp->length < sizeof(*rsdp) || rsdp->length > X86_ACPI_RSDP_MAX_LENGTH) return false;
	return acpi_checksum_valid(rsdp, (size_t)rsdp->length);
}

static bool acpi_sdt_valid(const struct x86_acpi_sdt_header* table) {
	if (table == NULL || table->length < sizeof(*table) || table->length > X86_ACPI_SDT_MAX_LENGTH) return false;
	return acpi_checksum_valid(table, (size_t)table->length);
}

static const struct x86_acpi_sdt_header* acpi_find_table(const char signature[4]) {
	uintptr_t rsdp_address;

	if (!kernel_boot_rsdp_address(&rsdp_address) || !boot_address_space_available()) return NULL;

	const struct x86_acpi_rsdp* rsdp = (const struct x86_acpi_rsdp*)rsdp_address;
	if (!acpi_rsdp_valid(rsdp)) return NULL;

	if (rsdp->revision >= 2u && rsdp->xsdt_address != 0u) {
		const struct x86_acpi_sdt_header* xsdt =
			(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)rsdp->xsdt_address);
		if (!acpi_sdt_valid(xsdt) || !acpi_signature_equals(xsdt->signature, "XSDT")) return NULL;
		if (((size_t)xsdt->length - sizeof(*xsdt)) % sizeof(uint64_t) != 0u) return NULL;

		size_t          entry_count = (xsdt->length - sizeof(*xsdt)) / sizeof(uint64_t);
		const uint64_t* entries     = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));

		for (size_t i = 0; i < entry_count; i++) {
			if (entries[i] == 0u) continue;
			const struct x86_acpi_sdt_header* table =
				(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)entries[i]);
			if (acpi_sdt_valid(table) && acpi_signature_equals(table->signature, signature)) {
				return table;
			}
		}

		return NULL;
	}

	if (rsdp->rsdt_address == 0u) return NULL;

	const struct x86_acpi_sdt_header* rsdt =
		(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)rsdp->rsdt_address);
	if (!acpi_sdt_valid(rsdt) || !acpi_signature_equals(rsdt->signature, "RSDT")) return NULL;
	if (((size_t)rsdt->length - sizeof(*rsdt)) % sizeof(uint32_t) != 0u) return NULL;

	size_t          entry_count = (rsdt->length - sizeof(*rsdt)) / sizeof(uint32_t);
	const uint32_t* entries     = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));

	for (size_t i = 0; i < entry_count; i++) {
		if (entries[i] == 0u) continue;
		const struct x86_acpi_sdt_header* table =
			(const struct x86_acpi_sdt_header*)hhdm_phys_to_virt((uintptr_t)entries[i]);
		if (acpi_sdt_valid(table) && acpi_signature_equals(table->signature, signature)) {
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

static void lapic_wait_icr_idle(void) {
	while ((lapic_read(X86_LAPIC_ICR_LOW_REG) & X86_LAPIC_ICR_DELIVERY_PENDING) != 0u) {
		__asm__ volatile("pause");
	}
}

static uint32_t ioapic_read(uint8_t reg) {
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_REGSEL) = reg;
	return *(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_WINDOW);
}

static void ioapic_write(uint8_t reg, uint32_t value) {
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_REGSEL) = reg;
	*(volatile uint32_t*)(ioapic_mmio + X86_IOAPIC_WINDOW) = value;
}

bool apic_init_local(void) {
	uint64_t apic_base;

	if (lapic_mmio == NULL) return false;

	apic_base = read_msr(X86_IA32_APIC_BASE_MSR);
	if ((apic_base & X86_IA32_APIC_BASE_ENABLE) == 0u) {
		write_msr(X86_IA32_APIC_BASE_MSR, apic_base | X86_IA32_APIC_BASE_ENABLE);
	}

	lapic_write(X86_LAPIC_TPR_REG, 0u);
	lapic_write(X86_LAPIC_SVR_REG, X86_LAPIC_SVR_ENABLE | X86_LAPIC_SPURIOUS_VECTOR);
	return true;
}

bool apic_ipi_ready(void) {
	return __atomic_load_n(&lapic_ready, __ATOMIC_ACQUIRE);
}

static bool lapic_init(uintptr_t lapic_phys) {
	if (lapic_mmio == NULL) {
		uint64_t apic_base = read_msr(X86_IA32_APIC_BASE_MSR);

		if (lapic_phys == 0u) {
			lapic_phys = (uintptr_t)(apic_base & X86_IA32_APIC_BASE_ADDR_MASK);
		}
		if (lapic_phys == 0u || !boot_address_space_available()) return false;
		if (!map_mmio_page(lapic_phys)) return false;

		lapic_mmio = (volatile uint8_t*)hhdm_phys_to_virt(lapic_phys);
	}

	if (!apic_init_local()) return false;
	__atomic_store_n(&lapic_ready, true, __ATOMIC_RELEASE);
	return true;
}

bool apic_prepare_ipi(void) {
	return lapic_init(0u);
}

bool apic_route_isa_irq(unsigned irq, unsigned vector) {
	const struct x86_acpi_sdt_header* madt_header = acpi_find_table("APIC");
	if (!madt_header || madt_header->length < sizeof(struct x86_acpi_madt)) return false;

	const struct x86_acpi_madt* madt            = (const struct x86_acpi_madt*)madt_header;
	uintptr_t                   lapic_phys      = (uintptr_t)madt->lapic_address;
	uintptr_t                   ioapic_phys     = 0u;
	uint32_t                    ioapic_gsi_base = 0u;
	uint32_t                    routed_gsi      = irq;
	uint16_t                    routed_flags    = 0u;
	const uint8_t*              entry           = (const uint8_t*)madt + sizeof(*madt);
	const uint8_t*              end             = (const uint8_t*)madt + madt->header.length;

	while (entry < end) {
		size_t remaining = (size_t)(end - entry);
		if (remaining < sizeof(struct x86_acpi_madt_entry_header)) return false;

		const struct x86_acpi_madt_entry_header* header = (const struct x86_acpi_madt_entry_header*)entry;
		if (header->length < sizeof(*header) || (size_t)header->length > remaining) return false;

		switch (header->type) {
		case X86_ACPI_MADT_TYPE_IO_APIC: {
			if (header->length < sizeof(struct x86_acpi_madt_io_apic)) return false;
			const struct x86_acpi_madt_io_apic* io_apic = (const struct x86_acpi_madt_io_apic*)entry;
			if (ioapic_phys == 0u) {
				ioapic_phys     = (uintptr_t)io_apic->io_apic_address;
				ioapic_gsi_base = io_apic->global_system_interrupt_base;
			}
			break;
		}
		case X86_ACPI_MADT_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
			if (header->length < sizeof(struct x86_acpi_madt_iso)) return false;
			const struct x86_acpi_madt_iso* iso = (const struct x86_acpi_madt_iso*)entry;
			if (iso->bus == 0u && iso->source == irq) {
				routed_gsi   = iso->global_system_interrupt;
				routed_flags = iso->flags;
			}
			break;
		}
		case X86_ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
			if (header->length < sizeof(struct x86_acpi_madt_lapic_addr_override)) return false;
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
	if (!apic_ipi_ready()) return;
	lapic_write(X86_LAPIC_EOI_REG, 0u);
}

bool apic_send_ipi(uint32_t lapic_id, unsigned vector) {
	if (!apic_ipi_ready() || lapic_mmio == NULL || vector >= 256u) return false;

	lapic_wait_icr_idle();
	lapic_write(X86_LAPIC_ICR_HIGH_REG, lapic_id << 24);
	lapic_write(X86_LAPIC_ICR_LOW_REG, (uint32_t)vector);
	lapic_wait_icr_idle();
	return true;
}
