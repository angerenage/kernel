#include "ranges.h"

#include <protocol/device.h>
#include <runtime/device.h>
#include <stdlib.h>
#include <string.h>

#define DT_PCI_ADDRESS_CELLS 3u
#define DT_PCI_PARENT_ADDRESS_CELLS 2u
#define DT_PCI_SIZE_CELLS 2u
#define DT_PCI_RANGE_CELLS (DT_PCI_ADDRESS_CELLS + DT_PCI_PARENT_ADDRESS_CELLS + DT_PCI_SIZE_CELLS)
#define DT_PCI_RANGE_SIZE (DT_PCI_RANGE_CELLS * sizeof(uint32_t))

#define DT_PCI_SPACE_MASK UINT32_C(0x03000000)
#define DT_PCI_SPACE_IO UINT32_C(0x01000000)
#define DT_PCI_SPACE_MEMORY32 UINT32_C(0x02000000)
#define DT_PCI_SPACE_MEMORY64 UINT32_C(0x03000000)
#define DT_PCI_PREFETCHABLE UINT32_C(0x40000000)

static uint32_t read_u32_be(const uint8_t* value) {
	return ((uint32_t)value[0] << 24u) | ((uint32_t)value[1] << 16u) | ((uint32_t)value[2] << 8u) | (uint32_t)value[3];
}

static uint64_t read_u64_cells(const uint8_t* value) {
	return ((uint64_t)read_u32_be(value) << 32u) | read_u32_be(value + sizeof(uint32_t));
}

static bool read_u32_property(cap_id_t device_cap, const char* name, uint32_t* out_value) {
	struct device_property_info info;
	uint8_t                     encoded[sizeof(uint32_t)];
	size_t                      name_size = strlen(name);

	if (out_value == NULL || device_property_get_info(device_cap, name, name_size, &info) != SYSCALL_STATUS_OK ||
	    info.type != DEVICE_PROPERTY_BYTES || info.element_count != sizeof(encoded) ||
	    info.value_size != sizeof(encoded) ||
	    device_property_read_bytes(device_cap, name, 0u, encoded, sizeof(encoded)) != SYSCALL_STATUS_OK)
		return false;
	*out_value = read_u32_be(encoded);
	return true;
}

void pci_host_ranges_initialize_identity(struct pci_host_ranges* ranges) {
	if (ranges != NULL) *ranges = (struct pci_host_ranges){.identity = true};
}

bool pci_host_ranges_read_dt(cap_id_t device_cap, struct pci_host_ranges* out_ranges) {
	struct device_property_info info;
	uint8_t*                    encoded;
	uint32_t                    address_cells;
	uint32_t                    size_cells;
	size_t                      count;

	if (out_ranges == NULL) return false;
	*out_ranges = (struct pci_host_ranges){0};
	if (!read_u32_property(device_cap, "address_cells", &address_cells) ||
	    !read_u32_property(device_cap, "size_cells", &size_cells) || address_cells != DT_PCI_ADDRESS_CELLS ||
	    size_cells != DT_PCI_SIZE_CELLS ||
	    device_property_get_info(device_cap, "ranges", sizeof("ranges") - 1u, &info) != SYSCALL_STATUS_OK ||
	    info.type != DEVICE_PROPERTY_BYTES || info.value_size == 0u || info.value_size > SIZE_MAX ||
	    info.value_size % DT_PCI_RANGE_SIZE != 0u)
		return false;
	count = info.value_size / DT_PCI_RANGE_SIZE;
	if (count == 0u || count > PCI_HOST_RANGE_COUNT || info.element_count != info.value_size) return false;
	encoded = malloc(info.value_size);
	if (encoded == NULL) return false;
	if (device_property_read_bytes(device_cap, "ranges", 0u, encoded, info.value_size) != SYSCALL_STATUS_OK) {
		free(encoded);
		return false;
	}
	for (size_t index = 0u; index < count; index++) {
		const uint8_t*         tuple = encoded + index * DT_PCI_RANGE_SIZE;
		uint32_t               flags = read_u32_be(tuple);
		uint32_t               space = flags & DT_PCI_SPACE_MASK;
		struct pci_host_range* range = &out_ranges->values[index];

		if (space == DT_PCI_SPACE_IO) range->kind = PCI_HOST_RANGE_IO;
		else if (space == DT_PCI_SPACE_MEMORY32 || space == DT_PCI_SPACE_MEMORY64) range->kind = PCI_HOST_RANGE_MEMORY;
		else {
			free(encoded);
			*out_ranges = (struct pci_host_ranges){0};
			return false;
		}
		range->bus_address = read_u64_cells(tuple + sizeof(uint32_t));
		range->cpu_address = read_u64_cells(tuple + DT_PCI_ADDRESS_CELLS * sizeof(uint32_t));
		range->size = read_u64_cells(tuple + (DT_PCI_ADDRESS_CELLS + DT_PCI_PARENT_ADDRESS_CELLS) * sizeof(uint32_t));
		range->prefetchable = (flags & DT_PCI_PREFETCHABLE) != 0u;
		if (range->size == 0u || range->bus_address > UINT64_MAX - range->size ||
		    range->cpu_address > UINT64_MAX - range->size) {
			free(encoded);
			*out_ranges = (struct pci_host_ranges){0};
			return false;
		}
	}
	free(encoded);
	out_ranges->count = count;
	return true;
}

bool pci_host_ranges_locate(const struct pci_host_ranges* ranges, enum pci_host_range_kind kind, uint64_t bus_address,
                            uint64_t size, size_t* out_index, uint64_t* out_offset, uint64_t* out_cpu_address) {
	bool matched = false;

	if (ranges == NULL || size == 0u || out_index == NULL || out_offset == NULL || out_cpu_address == NULL)
		return false;
	if (ranges->identity) {
		*out_index       = 0u;
		*out_offset      = bus_address;
		*out_cpu_address = bus_address;
		return bus_address <= UINT64_MAX - size;
	}
	for (size_t index = 0u; index < ranges->count; index++) {
		const struct pci_host_range* range = &ranges->values[index];
		uint64_t                     offset;

		if (range->kind != kind || bus_address < range->bus_address) continue;
		offset = bus_address - range->bus_address;
		if (offset > range->size || size > range->size - offset) continue;
		if (matched || range->cpu_address > UINT64_MAX - offset) return false;
		*out_index       = index;
		*out_offset      = offset;
		*out_cpu_address = range->cpu_address + offset;
		matched          = true;
	}
	return matched;
}
