#include <firmware/acpi.h>
#include <kernel/hardware/pci.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_ECAM_BUS_SIZE (1ull << 20u)

struct acpi_mcfg {
	struct acpi_sdt_header header;
	uint64_t               reserved;
	uint8_t                allocations[];
} __attribute__((packed));

struct acpi_mcfg_allocation {
	uint64_t address;
	uint16_t segment_group;
	uint8_t  start_bus;
	uint8_t  end_bus;
	uint32_t reserved;
} __attribute__((packed));

static bool pci_mcfg_table_valid(const struct acpi_mcfg* table) {
	return table != NULL && table->header.length >= sizeof(*table) && table->reserved == 0u &&
	       ((size_t)table->header.length - sizeof(*table)) % sizeof(struct acpi_mcfg_allocation) == 0u;
}

static bool pci_mcfg_allocation(const struct acpi_mcfg_allocation* allocation, struct pci_controller* out_controller) {
	uint64_t offset;
	uint64_t size;

	if (allocation->address == 0u || (allocation->address & (PCI_ECAM_BUS_SIZE - 1u)) != 0u ||
	    allocation->start_bus > allocation->end_bus || allocation->reserved != 0u)
		return false;
	offset = (uint64_t)allocation->start_bus * PCI_ECAM_BUS_SIZE;
	size   = ((uint64_t)allocation->end_bus - allocation->start_bus + 1u) * PCI_ECAM_BUS_SIZE;
	if (allocation->address > UINT64_MAX - offset || allocation->address + offset > UINT64_MAX - size) return false;
	if (out_controller != NULL) {
		*out_controller = (struct pci_controller){
			.register_address = allocation->address + offset,
			.register_size    = size,
			.access           = PCI_CONFIG_ACCESS_ECAM,
			.segment_group    = allocation->segment_group,
			.start_bus        = allocation->start_bus,
			.end_bus          = allocation->end_bus,
		};
	}
	return true;
}

static bool pci_mcfg_find(size_t target, struct pci_controller* out_controller, size_t* out_count) {
	acpi_cursor_t cursor = ACPI_CURSOR_INIT;
	size_t        count  = 0u;

	for (;;) {
		const struct acpi_mcfg* table = (const struct acpi_mcfg*)acpi_table_next("MCFG", &cursor);

		if (table == NULL) {
			if (out_count != NULL) *out_count = count;
			return false;
		}
		if (!pci_mcfg_table_valid(table)) continue;
		for (size_t offset = sizeof(*table); offset < table->header.length;
		     offset += sizeof(struct acpi_mcfg_allocation)) {
			const struct acpi_mcfg_allocation* allocation =
				(const struct acpi_mcfg_allocation*)((const uint8_t*)table + offset);
			struct pci_controller controller;

			if (!pci_mcfg_allocation(allocation, &controller)) continue;
			if (count == target && out_controller != NULL) {
				*out_controller = controller;
				return true;
			}
			if (count == SIZE_MAX) {
				if (out_count != NULL) *out_count = count;
				return false;
			}
			count++;
		}
	}
}

size_t kernel_hardware_pci_count(void) {
	size_t count = 0u;

	(void)pci_mcfg_find(SIZE_MAX, NULL, &count);
	return count;
}

bool kernel_hardware_pci_get(size_t index, struct pci_controller* out_controller) {
	return out_controller != NULL && pci_mcfg_find(index, out_controller, NULL);
}
