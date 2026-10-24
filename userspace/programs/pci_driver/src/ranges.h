#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_HOST_RANGE_COUNT 8u

/* Address space described by one PCI host bridge range. */
enum pci_host_range_kind {
	PCI_HOST_RANGE_IO = 0u,
	PCI_HOST_RANGE_MEMORY,
};

/* One PCI bus-address window and its corresponding CPU physical window. */
struct pci_host_range {
	enum pci_host_range_kind kind;
	uint64_t                 bus_address;
	uint64_t                 cpu_address;
	uint64_t                 size;
	bool                     prefetchable;
};

/* Address translations supplied by the firmware description of a PCI host. */
struct pci_host_ranges {
	struct pci_host_range values[PCI_HOST_RANGE_COUNT];
	size_t                count;
	bool                  identity;
};

/* Configure identity translation for a host described by PC ACPI tables. */
void pci_host_ranges_initialize_identity(struct pci_host_ranges* ranges);

/* Decode the standard PCI `ranges` property of a Device Tree host. */
bool pci_host_ranges_read_dt(cap_id_t device_cap, struct pci_host_ranges* out_ranges);

/* Locate the unique host window containing a complete PCI bus-address range. */
bool pci_host_ranges_locate(const struct pci_host_ranges* ranges, enum pci_host_range_kind kind, uint64_t bus_address,
                            uint64_t size, size_t* out_index, uint64_t* out_offset, uint64_t* out_cpu_address);
