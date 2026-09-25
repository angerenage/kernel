#include "apic.h"

#include <hal/interrupts.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../utils.h"
#include "vectors.h"

#define X86_PAGE_SIZE 0x1000u
#define X86_IA32_APIC_BASE_MSR 0x1bu
#define X86_IA32_APIC_BASE_ENABLE (1ull << 11)
#define X86_IA32_APIC_BASE_ADDR_MASK 0xfffff000ull
#define X86_LAPIC_ID_REG 0x20u
#define X86_LAPIC_VERSION_REG 0x30u
#define X86_LAPIC_TPR_REG 0x80u
#define X86_LAPIC_EOI_REG 0x0b0u
#define X86_LAPIC_ICR_LOW_REG 0x300u
#define X86_LAPIC_ICR_HIGH_REG 0x310u
#define X86_LAPIC_LVT_TIMER_REG 0x320u
#define X86_LAPIC_LVT_THERMAL_REG 0x330u
#define X86_LAPIC_LVT_PERF_REG 0x340u
#define X86_LAPIC_LVT_ERROR_REG 0x370u
#define X86_LAPIC_ICR_DELIVERY_PENDING (1u << 12)
#define X86_LAPIC_SVR_REG 0x0f0u
#define X86_LAPIC_SVR_ENABLE 0x100u
#define X86_LAPIC_LVT_MASKED (1u << 16)
#define X86_IOAPIC_REGSEL 0x00u
#define X86_IOAPIC_WINDOW 0x10u
#define X86_IOAPIC_VERSION_REG 0x01u
#define X86_IOAPIC_REDIR_BASE 0x10u
#define X86_IOAPIC_REDIR_POLARITY_LOW (1ull << 13)
#define X86_IOAPIC_REDIR_TRIGGER_LEVEL (1ull << 15)
#define X86_IOAPIC_REDIR_MASK (1ull << 16)
#define X86_ACPI_MADT_TYPE_IO_APIC 1u
#define X86_ACPI_MADT_TYPE_INTERRUPT_SOURCE_OVERRIDE 2u
#define X86_ACPI_MADT_TYPE_LAPIC_ADDR_OVERRIDE 5u
#define X86_ACPI_MADT_POLARITY_MASK 0x0003u
#define X86_ACPI_MADT_POLARITY_ACTIVE_LOW 0x0003u
#define X86_ACPI_MADT_TRIGGER_MASK 0x000cu
#define X86_ACPI_MADT_TRIGGER_LEVEL 0x000cu

#define X86_ACPI_RSDP_V1_LENGTH 20u
#define X86_ACPI_RSDP_MAX_LENGTH 4096u
#define X86_ACPI_SDT_MAX_LENGTH (16u * 1024u * 1024u)
#define X86_IOAPIC_MAX_CONTROLLERS 16u

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
static uint32_t          ioapic_register_lock;
static bool              ioapic_probed;

static bool lapic_init(uintptr_t lapic_phys);

struct x86_isa_route {
	uintptr_t         lapic_phys;
	volatile uint8_t* registers;
	uint32_t          index;
	uint16_t          flags;
	bool              available;
};

static struct x86_isa_route isa_routes[X86_IRQ_COUNT];

struct x86_ioapic {
	uintptr_t         physical_address;
	uintptr_t         lapic_phys;
	volatile uint8_t* registers;
	uint32_t          redirection_count;
};

static struct x86_ioapic ioapics[X86_IOAPIC_MAX_CONTROLLERS];
static size_t            ioapic_count;

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

	if (hal_paging_query(hal_paging_kernel_space(), virt, NULL)) return true;

	return hal_paging_map(hal_paging_kernel_space(),
	                      &(const struct hal_paging_map_request){
							  virt,
							  phys & ~(uintptr_t)(X86_PAGE_SIZE - 1u),
							  X86_PAGE_SIZE,
							  HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL,
							  MEMORY_TYPE_DEVICE,
						  });
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

static struct irq_state ioapic_lock_acquire(void) {
	struct irq_state irq = irq_save_disable();
	while (__atomic_exchange_n(&ioapic_register_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("pause");
	return irq;
}

static void ioapic_lock_release(struct irq_state irq) {
	__atomic_store_n(&ioapic_register_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(irq);
}

static uint32_t ioapic_read(volatile uint8_t* registers, uint8_t reg) {
	*(volatile uint32_t*)(registers + X86_IOAPIC_REGSEL) = reg;
	return *(volatile uint32_t*)(registers + X86_IOAPIC_WINDOW);
}

static void ioapic_write(volatile uint8_t* registers, uint8_t reg, uint32_t value) {
	*(volatile uint32_t*)(registers + X86_IOAPIC_REGSEL) = reg;
	*(volatile uint32_t*)(registers + X86_IOAPIC_WINDOW) = value;
}

bool apic_init_local(void) {
	uint64_t apic_base;

	if (lapic_mmio == NULL) return lapic_init(0u);

	apic_base = read_msr(X86_IA32_APIC_BASE_MSR);
	if ((apic_base & X86_IA32_APIC_BASE_ENABLE) == 0u) {
		write_msr(X86_IA32_APIC_BASE_MSR, apic_base | X86_IA32_APIC_BASE_ENABLE);
	}

	/* Block maskable local delivery while discarding firmware LVT state. */
	lapic_write(X86_LAPIC_TPR_REG, 0xffu);
	uint32_t max_lvt = (lapic_read(X86_LAPIC_VERSION_REG) >> 16u) & 0xffu;
	lapic_write(X86_LAPIC_LVT_TIMER_REG, lapic_read(X86_LAPIC_LVT_TIMER_REG) | X86_LAPIC_LVT_MASKED);
	if (max_lvt >= 3u) lapic_write(X86_LAPIC_LVT_ERROR_REG, lapic_read(X86_LAPIC_LVT_ERROR_REG) | X86_LAPIC_LVT_MASKED);
	if (max_lvt >= 4u) lapic_write(X86_LAPIC_LVT_PERF_REG, lapic_read(X86_LAPIC_LVT_PERF_REG) | X86_LAPIC_LVT_MASKED);
	if (max_lvt >= 5u)
		lapic_write(X86_LAPIC_LVT_THERMAL_REG, lapic_read(X86_LAPIC_LVT_THERMAL_REG) | X86_LAPIC_LVT_MASKED);
	lapic_write(X86_LAPIC_SVR_REG, X86_LAPIC_SVR_ENABLE | X86_LAPIC_SPURIOUS_VECTOR);
	lapic_write(X86_LAPIC_TPR_REG, 0u);
	__atomic_store_n(&lapic_ready, true, __ATOMIC_RELEASE);
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
	return true;
}

bool apic_prepare_ipi(void) {
	return lapic_init(0u);
}

bool apic_probe_isa_irqs(void) {
	if (ioapic_probed) return true;
	for (uint32_t irq = 0u; irq < X86_IRQ_COUNT; irq++) isa_routes[irq] = (struct x86_isa_route){0};
	memset(ioapics, 0, sizeof(ioapics));
	ioapic_count                                  = 0u;
	const struct x86_acpi_sdt_header* madt_header = acpi_find_table("APIC");
	if (!madt_header || madt_header->length < sizeof(struct x86_acpi_madt)) {
		ioapic_probed = true;
		return true;
	}

	const struct x86_acpi_madt* madt       = (const struct x86_acpi_madt*)madt_header;
	uintptr_t                   lapic_phys = (uintptr_t)madt->lapic_address;
	uint32_t                    routed_gsi[X86_IRQ_COUNT];
	uint16_t                    routed_flags[X86_IRQ_COUNT];
	bool                        duplicate[X86_IRQ_COUNT] = {0};
	for (uint32_t irq = 0u; irq < X86_IRQ_COUNT; irq++) {
		routed_gsi[irq]   = irq;
		routed_flags[irq] = 0u;
	}
	const uint8_t* entry = (const uint8_t*)madt + sizeof(*madt);
	const uint8_t* end   = (const uint8_t*)madt + madt->header.length;

	while (entry < end) {
		size_t remaining = (size_t)(end - entry);
		if (remaining < sizeof(struct x86_acpi_madt_entry_header)) return false;

		const struct x86_acpi_madt_entry_header* header = (const struct x86_acpi_madt_entry_header*)entry;
		if (header->length < sizeof(*header) || (size_t)header->length > remaining) return false;

		switch (header->type) {
		case X86_ACPI_MADT_TYPE_IO_APIC: {
			if (header->length < sizeof(struct x86_acpi_madt_io_apic)) return false;
			break;
		}
		case X86_ACPI_MADT_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
			if (header->length < sizeof(struct x86_acpi_madt_iso)) return false;
			const struct x86_acpi_madt_iso* iso = (const struct x86_acpi_madt_iso*)entry;
			if (iso->bus == 0u && iso->source < X86_IRQ_COUNT) {
				routed_gsi[iso->source]   = iso->global_system_interrupt;
				routed_flags[iso->source] = iso->flags;
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

	for (entry = (const uint8_t*)madt + sizeof(*madt); entry < end;
	     entry += ((const struct x86_acpi_madt_entry_header*)entry)->length) {
		const struct x86_acpi_madt_entry_header* header = (const struct x86_acpi_madt_entry_header*)entry;
		if (header->type != X86_ACPI_MADT_TYPE_IO_APIC) continue;
		const struct x86_acpi_madt_io_apic* io_apic = (const struct x86_acpi_madt_io_apic*)entry;
		uintptr_t                           address = (uintptr_t)io_apic->io_apic_address;
		if (address == 0u || !map_mmio_page(address)) return false;
		volatile uint8_t* registers   = (volatile uint8_t*)hhdm_phys_to_virt(address);
		struct irq_state  irq         = ioapic_lock_acquire();
		uint32_t          redir_count = ((ioapic_read(registers, X86_IOAPIC_VERSION_REG) >> 16) & 0xffu) + 1u;
		if (redir_count > 120u) {
			ioapic_lock_release(irq);
			return false;
		}
		for (uint32_t route = 0u; route < redir_count; route++) {
			uint8_t  low_reg = (uint8_t)(X86_IOAPIC_REDIR_BASE + route * 2u);
			uint32_t low     = ioapic_read(registers, low_reg);
			ioapic_write(registers, low_reg, low | X86_IOAPIC_REDIR_MASK);
		}
		ioapic_lock_release(irq);
		uint32_t base = io_apic->global_system_interrupt_base;
		if (ioapic_count == X86_IOAPIC_MAX_CONTROLLERS) return false;
		for (size_t previous = 0u; previous < ioapic_count; previous++)
			if (ioapics[previous].physical_address == address) return false;
		ioapics[ioapic_count++] = (struct x86_ioapic){.physical_address  = address,
		                                              .lapic_phys        = lapic_phys,
		                                              .registers         = registers,
		                                              .redirection_count = redir_count};
		for (uint32_t irq = 0u; irq < X86_IRQ_COUNT; irq++) {
			if (routed_gsi[irq] < base || routed_gsi[irq] - base >= redir_count) continue;
			if (isa_routes[irq].available) {
				duplicate[irq] = true;
				continue;
			}
			isa_routes[irq] = (struct x86_isa_route){.lapic_phys = lapic_phys,
			                                         .registers  = registers,
			                                         .index      = routed_gsi[irq] - base,
			                                         .flags      = routed_flags[irq],
			                                         .available  = true};
		}
	}
	for (uint32_t irq = 0u; irq < X86_IRQ_COUNT; irq++)
		if (duplicate[irq] || isa_routes[irq].index >= 120u) isa_routes[irq].available = false;
	ioapic_probed = true;
	return true;
}

bool apic_isa_irq_available(unsigned irq) {
	return irq < X86_IRQ_COUNT && ioapic_probed && isa_routes[irq].available;
}

bool apic_resolve_ioapic_source(uint64_t controller_address, uint32_t local_source_id,
                                struct hal_interrupt_source* out_source) {
	if (out_source == NULL || controller_address > UINTPTR_MAX || (!ioapic_probed && !apic_probe_isa_irqs()))
		return false;
	for (size_t index = 0u; index < ioapic_count; index++) {
		if (ioapics[index].physical_address != (uintptr_t)controller_address ||
		    local_source_id >= ioapics[index].redirection_count)
			continue;
		for (uint32_t irq = 0u; irq < X86_IRQ_COUNT; irq++) {
			if (!isa_routes[irq].available || isa_routes[irq].registers != ioapics[index].registers ||
			    isa_routes[irq].index != local_source_id)
				continue;
			/* IRQ0 is owned by the kernel clock and IRQ2 is the legacy cascade. */
			if (irq == 0u || irq == 2u) return false;
			*out_source = (struct hal_interrupt_source){.domain = 0u, .number = irq};
			return true;
		}
		*out_source = (struct hal_interrupt_source){.domain = (uint32_t)index + 1u, .number = local_source_id};
		return true;
	}
	return false;
}

bool apic_ioapic_source_available(const struct hal_interrupt_source* source) {
	if (source == NULL || source->domain == 0u || (!ioapic_probed && !apic_probe_isa_irqs())) return false;
	size_t index = (size_t)source->domain - 1u;
	return index < ioapic_count && source->number < ioapics[index].redirection_count;
}

static bool apic_route_ioapic(const struct x86_ioapic* selected, uint32_t route, uint16_t flags, unsigned vector,
                              uint32_t target_lapic_id, enum hal_interrupt_trigger trigger,
                              enum hal_interrupt_polarity polarity, uint32_t* out_route, uintptr_t* out_registers) {
	if (selected == NULL || route >= selected->redirection_count || vector < 32u || vector >= 255u ||
	    target_lapic_id > 255u || trigger > HAL_INTERRUPT_TRIGGER_LEVEL || polarity > HAL_INTERRUPT_POLARITY_LOW ||
	    out_route == NULL || out_registers == NULL || !lapic_init(selected->lapic_phys))
		return false;
	uint64_t redir             = (uint64_t)vector | X86_IOAPIC_REDIR_MASK;
	uint16_t firmware_polarity = flags & X86_ACPI_MADT_POLARITY_MASK;
	uint16_t firmware_trigger  = flags & X86_ACPI_MADT_TRIGGER_MASK;
	if (firmware_polarity == 2u || firmware_trigger == 8u) return false;
	bool active_low =
		polarity == HAL_INTERRUPT_POLARITY_LOW ||
		(polarity == HAL_INTERRUPT_POLARITY_FIRMWARE && firmware_polarity == X86_ACPI_MADT_POLARITY_ACTIVE_LOW);
	bool level = trigger == HAL_INTERRUPT_TRIGGER_LEVEL ||
	             (trigger == HAL_INTERRUPT_TRIGGER_FIRMWARE && firmware_trigger == X86_ACPI_MADT_TRIGGER_LEVEL);
	if (active_low) redir |= X86_IOAPIC_REDIR_POLARITY_LOW;
	if (level) redir |= X86_IOAPIC_REDIR_TRIGGER_LEVEL;
	struct irq_state route_irq_state = ioapic_lock_acquire();
	uint8_t          low_reg         = (uint8_t)(X86_IOAPIC_REDIR_BASE + route * 2u);
	uint32_t         current_low     = ioapic_read(selected->registers, low_reg);
	ioapic_write(selected->registers, low_reg, current_low | X86_IOAPIC_REDIR_MASK);
	ioapic_write(selected->registers, (uint8_t)(low_reg + 1u), target_lapic_id << 24);
	ioapic_write(selected->registers, low_reg, (uint32_t)redir);
	ioapic_lock_release(route_irq_state);
	*out_route     = route;
	*out_registers = (uintptr_t)selected->registers;
	apic_active    = true;
	return true;
}

bool apic_route_isa_irq(unsigned irq, unsigned vector, uint32_t target_lapic_id, enum hal_interrupt_trigger trigger,
                        enum hal_interrupt_polarity polarity, uint32_t* out_route, uintptr_t* out_registers) {
	if (irq >= X86_IRQ_COUNT || vector < 32u || vector >= 255u || out_route == NULL || out_registers == NULL ||
	    target_lapic_id > 255u || trigger > HAL_INTERRUPT_TRIGGER_LEVEL || polarity > HAL_INTERRUPT_POLARITY_LOW ||
	    (!ioapic_probed && !apic_probe_isa_irqs()) || !isa_routes[irq].available)
		return false;
	const struct x86_isa_route* selected = &isa_routes[irq];
	struct x86_ioapic           ioapic   = {
		.lapic_phys = selected->lapic_phys, .registers = selected->registers, .redirection_count = 120u};
	return apic_route_ioapic(&ioapic,
	                         selected->index,
	                         selected->flags,
	                         vector,
	                         target_lapic_id,
	                         trigger,
	                         polarity,
	                         out_route,
	                         out_registers);
}

bool apic_route_ioapic_source(const struct hal_interrupt_source* source, unsigned vector, uint32_t target_lapic_id,
                              enum hal_interrupt_trigger trigger, enum hal_interrupt_polarity polarity,
                              uint32_t* out_route, uintptr_t* out_registers) {
	if (!apic_ioapic_source_available(source)) return false;
	return apic_route_ioapic(&ioapics[source->domain - 1u],
	                         source->number,
	                         0u,
	                         vector,
	                         target_lapic_id,
	                         trigger,
	                         polarity,
	                         out_route,
	                         out_registers);
}

bool apic_set_isa_irq_mask(uintptr_t registers_address, uint32_t route, bool masked) {
	uint8_t           low_reg;
	uint32_t          low_value;
	volatile uint8_t* registers = (volatile uint8_t*)registers_address;

	if (!apic_active || registers == NULL || route >= 120u) return false;

	struct irq_state irq = ioapic_lock_acquire();
	low_reg              = (uint8_t)(X86_IOAPIC_REDIR_BASE + route * 2u);
	low_value            = ioapic_read(registers, low_reg);
	if (masked) {
		low_value |= (uint32_t)X86_IOAPIC_REDIR_MASK;
	}
	else {
		low_value &= ~(uint32_t)X86_IOAPIC_REDIR_MASK;
	}
	ioapic_write(registers, low_reg, low_value);
	ioapic_lock_release(irq);
	return true;
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

bool apic_send_nmi(uint32_t lapic_id) {
	if (!apic_ipi_ready() || lapic_mmio == NULL) return false;

	lapic_wait_icr_idle();
	lapic_write(X86_LAPIC_ICR_HIGH_REG, lapic_id << 24);
	lapic_write(X86_LAPIC_ICR_LOW_REG, 4u << 8);
	lapic_wait_icr_idle();
	return true;
}
