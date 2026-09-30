#include "pci.h"

#include <base/device.h>
#include <base/hardware/pci.h>
#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <kernel/device.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PCI_ECAM_BUS_SIZE (1ull << 20u)
#define PCI_DT_COMPATIBLE "pci-host-ecam-generic"

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

struct pci_candidate {
	struct pci_controller controller;
	bool                  domain_explicit;
};

enum pci_candidate_relation {
	PCI_CANDIDATE_DISTINCT,
	PCI_CANDIDATE_DUPLICATE,
	PCI_CANDIDATE_CONFLICT,
};

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
			.domain           = allocation->segment_group,
			.start_bus        = allocation->start_bus,
			.end_bus          = allocation->end_bus,
		};
	}
	return true;
}

static bool pci_mcfg_find(size_t target, struct pci_candidate* out_candidate, size_t* out_count) {
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
			if (count == target && out_candidate != NULL) {
				*out_candidate = (struct pci_candidate){.controller = controller, .domain_explicit = true};
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

static bool pci_dt_u32(struct dt_node node, const char* name, bool* out_present, uint32_t* out_value) {
	struct dt_property property;
	uint64_t           value;

	if (out_present == NULL || out_value == NULL) return false;
	if (!dt_node_property(node, name, &property)) {
		*out_present = false;
		return true;
	}
	if (property.size != 4u || !dt_property_read_cells(&property, 0u, 1u, &value)) return false;
	*out_present = true;
	*out_value   = (uint32_t)value;
	return true;
}

static bool pci_dt_controller(struct dt_node node, struct pci_controller* out_controller) {
	struct dt_property device_type;
	struct dt_property ranges;
	struct dt_property bus_range;
	struct dt_reg      reg;
	uint64_t           start_bus = 0u;
	uint64_t           end_bus;
	uint64_t           size;
	uint64_t           available_buses;
	bool               present;
	uint32_t           cells;

	if (!dt_node_property(node, "device_type", &device_type) || device_type.size != sizeof("pci") ||
	    memcmp(device_type.data, "pci", sizeof("pci")) != 0 || !pci_dt_u32(node, "#address-cells", &present, &cells) ||
	    !present || cells != 3u || !pci_dt_u32(node, "#size-cells", &present, &cells) || !present || cells != 2u ||
	    !dt_node_property(node, "ranges", &ranges) || ranges.size == 0u || !dt_node_reg(node, 0u, &reg) ||
	    reg.address == 0u || reg.size < PCI_ECAM_BUS_SIZE)
		return false;
	if (dt_node_property(node, "bus-range", &bus_range)) {
		if (bus_range.size != 8u || !dt_property_read_cells(&bus_range, 0u, 1u, &start_bus) ||
		    !dt_property_read_cells(&bus_range, 1u, 1u, &end_bus) || start_bus > UINT8_MAX || end_bus > UINT8_MAX ||
		    start_bus > end_bus)
			return false;
	}
	else {
		available_buses = reg.size / PCI_ECAM_BUS_SIZE;
		end_bus         = available_buses > 256u ? UINT8_MAX : available_buses - 1u;
	}
	size = (end_bus - start_bus + 1u) * PCI_ECAM_BUS_SIZE;
	if (reg.size < size || reg.address > UINT64_MAX - size) return false;
	if (out_controller != NULL) {
		*out_controller = (struct pci_controller){
			.register_address = reg.address,
			.register_size    = size,
			.access           = PCI_CONFIG_ACCESS_ECAM,
			.start_bus        = (uint8_t)start_bus,
			.end_bus          = (uint8_t)end_bus,
		};
	}
	return true;
}

static bool pci_dt_domain(struct dt_node node, bool* out_present, uint32_t* out_domain) {
	uint32_t value;

	if (!pci_dt_u32(node, "linux,pci-domain", out_present, &value)) return false;
	if (*out_present && out_domain != NULL) *out_domain = value;
	return true;
}

static bool pci_dt_domains(bool* out_explicit) {
	bool   found    = false;
	bool   explicit = false;
	size_t devices  = dt_device_count(PCI_DT_COMPATIBLE);

	if (out_explicit == NULL) return false;
	for (size_t index = 0u; index < devices; index++) {
		struct dt_node node = dt_device_at(PCI_DT_COMPATIBLE, index);
		uint32_t       domain;
		bool           present;

		if (!pci_dt_controller(node, NULL) || !pci_dt_domain(node, &present, &domain)) continue;
		if (found && present != explicit) return false;
		found    = true;
		explicit = present;
		if (!present) continue;
		for (size_t previous = 0u; previous < index; previous++) {
			struct dt_node previous_node = dt_device_at(PCI_DT_COMPATIBLE, previous);
			uint32_t       previous_domain;
			bool           previous_present;

			if (pci_dt_controller(previous_node, NULL) &&
			    pci_dt_domain(previous_node, &previous_present, &previous_domain) && previous_present &&
			    previous_domain == domain)
				return false;
		}
	}
	*out_explicit = explicit;
	return true;
}

static bool pci_dt_find(size_t target, struct pci_candidate* out_candidate, size_t* out_count) {
	bool   explicit_domains;
	size_t devices = dt_device_count(PCI_DT_COMPATIBLE);
	size_t count   = 0u;

	if (!pci_dt_domains(&explicit_domains)) {
		if (out_count != NULL) *out_count = 0u;
		return false;
	}
	for (size_t index = 0u; index < devices; index++) {
		struct dt_node        node = dt_device_at(PCI_DT_COMPATIBLE, index);
		struct pci_controller controller;
		uint32_t              domain;
		bool                  present;

		if (!pci_dt_controller(node, &controller) || !pci_dt_domain(node, &present, &domain) ||
		    present != explicit_domains || (!explicit_domains && count > UINT32_MAX))
			continue;
		controller.domain = explicit_domains ? domain : (uint32_t)count;
		if (count == target && out_candidate != NULL) {
			*out_candidate = (struct pci_candidate){.controller = controller, .domain_explicit = present};
			return true;
		}
		if (count == SIZE_MAX) break;
		count++;
	}
	if (out_count != NULL) *out_count = count;
	return false;
}

static size_t pci_candidate_count(void) {
	size_t dt_count   = 0u;
	size_t acpi_count = 0u;

	(void)pci_dt_find(SIZE_MAX, NULL, &dt_count);
	(void)pci_mcfg_find(SIZE_MAX, NULL, &acpi_count);
	return acpi_count > SIZE_MAX - dt_count ? SIZE_MAX : dt_count + acpi_count;
}

static bool pci_candidate_at(size_t index, struct pci_candidate* out_candidate) {
	size_t dt_count = 0u;

	if (out_candidate == NULL) return false;
	if (pci_dt_find(index, out_candidate, &dt_count)) return true;
	return index >= dt_count && pci_mcfg_find(index - dt_count, out_candidate, NULL);
}

static bool pci_controller_range_end(const struct pci_controller* controller, uint64_t* out_end) {
	if (controller == NULL || out_end == NULL || controller->register_size == 0u ||
	    controller->register_address > UINT64_MAX - controller->register_size)
		return false;
	*out_end = controller->register_address + controller->register_size;
	return true;
}

static enum pci_candidate_relation pci_candidate_relation(const struct pci_candidate* first,
                                                          const struct pci_candidate* second) {
	uint64_t first_end;
	uint64_t second_end;
	bool     same_range;
	bool     bus_overlap;

	if (first == NULL || second == NULL || !pci_controller_range_end(&first->controller, &first_end) ||
	    !pci_controller_range_end(&second->controller, &second_end))
		return PCI_CANDIDATE_CONFLICT;
	same_range  = first->controller.access == second->controller.access &&
	              first->controller.register_address == second->controller.register_address &&
	              first->controller.register_size == second->controller.register_size;
	bus_overlap = first->controller.start_bus <= second->controller.end_bus &&
	              second->controller.start_bus <= first->controller.end_bus;
	if (same_range) {
		if (first->controller.start_bus != second->controller.start_bus ||
		    first->controller.end_bus != second->controller.end_bus ||
		    (first->domain_explicit && second->domain_explicit &&
		     first->controller.domain != second->controller.domain))
			return PCI_CANDIDATE_CONFLICT;
		return PCI_CANDIDATE_DUPLICATE;
	}
	if (first->controller.register_address < second_end && second->controller.register_address < first_end)
		return PCI_CANDIDATE_CONFLICT;
	if (first->domain_explicit && second->domain_explicit && first->controller.domain == second->controller.domain &&
	    bus_overlap)
		return PCI_CANDIDATE_CONFLICT;
	return PCI_CANDIDATE_DISTINCT;
}

static bool pci_candidates_valid(size_t count) {
	for (size_t first_index = 0u; first_index < count; first_index++) {
		struct pci_candidate first;

		if (!pci_candidate_at(first_index, &first)) return false;
		for (size_t second_index = first_index + 1u; second_index < count; second_index++) {
			struct pci_candidate second;

			if (!pci_candidate_at(second_index, &second) ||
			    pci_candidate_relation(&first, &second) == PCI_CANDIDATE_CONFLICT)
				return false;
		}
	}
	return true;
}

static bool pci_candidate_is_first(size_t index, const struct pci_candidate* candidate) {
	for (size_t previous_index = 0u; previous_index < index; previous_index++) {
		struct pci_candidate previous;

		if (!pci_candidate_at(previous_index, &previous)) return false;
		if (pci_candidate_relation(&previous, candidate) == PCI_CANDIDATE_DUPLICATE) return false;
	}
	return true;
}

static bool pci_candidate_explicit_domain(size_t index, size_t count, const struct pci_candidate* candidate,
                                          bool* out_present, uint32_t* out_domain) {
	bool     present = false;
	uint32_t domain  = 0u;

	if (candidate == NULL || out_present == NULL || out_domain == NULL) return false;
	for (size_t candidate_index = index; candidate_index < count; candidate_index++) {
		struct pci_candidate matching;

		if (!pci_candidate_at(candidate_index, &matching)) return false;
		if (pci_candidate_relation(candidate, &matching) != PCI_CANDIDATE_DUPLICATE) continue;
		if (!matching.domain_explicit) continue;
		if (present && domain != matching.controller.domain) return false;
		present = true;
		domain  = matching.controller.domain;
	}
	*out_present = present;
	*out_domain  = domain;
	return true;
}

static bool pci_explicit_domain_used(size_t count, uint32_t domain) {
	for (size_t index = 0u; index < count; index++) {
		struct pci_candidate candidate;

		if (!pci_candidate_at(index, &candidate)) return true;
		if (candidate.domain_explicit && candidate.controller.domain == domain) return true;
	}
	return false;
}

static bool pci_implicit_domain(size_t count, size_t ordinal, uint32_t* out_domain) {
	uint32_t domain = 0u;

	if (out_domain == NULL) return false;
	for (;;) {
		if (!pci_explicit_domain_used(count, domain)) {
			if (ordinal == 0u) {
				*out_domain = domain;
				return true;
			}
			ordinal--;
		}
		if (domain == UINT32_MAX) return false;
		domain++;
	}
}

static bool pci_reconciled_find(size_t target, struct pci_controller* out_controller, size_t* out_count) {
	size_t candidate_total = pci_candidate_count();
	size_t output_index    = 0u;
	size_t implicit_index  = 0u;

	if (!pci_candidates_valid(candidate_total)) {
		if (out_count != NULL) *out_count = 0u;
		return false;
	}
	for (size_t index = 0u; index < candidate_total; index++) {
		struct pci_candidate candidate;
		bool                 domain_explicit;
		uint32_t             domain;

		if (!pci_candidate_at(index, &candidate) || !pci_candidate_is_first(index, &candidate)) continue;
		if (!pci_candidate_explicit_domain(index, candidate_total, &candidate, &domain_explicit, &domain)) {
			if (out_count != NULL) *out_count = 0u;
			return false;
		}
		if (!domain_explicit && !pci_implicit_domain(candidate_total, implicit_index++, &domain)) {
			if (out_count != NULL) *out_count = 0u;
			return false;
		}
		candidate.controller.domain = domain;
		if (output_index == target && out_controller != NULL) {
			*out_controller = candidate.controller;
			return true;
		}
		if (output_index == SIZE_MAX) break;
		output_index++;
	}
	if (out_count != NULL) *out_count = output_index;
	return false;
}

bool kernel_device_register_pci(void) {
	size_t count = 0u;

	if (!kernel_device_register_type(KERNEL_DEVICE_TYPE_PCI, sizeof(struct pci_controller))) return false;
	(void)pci_reconciled_find(SIZE_MAX, NULL, &count);
	for (size_t index = 0u; index < count; index++) {
		struct pci_controller controller;

		if (!pci_reconciled_find(index, &controller, NULL) ||
		    !kernel_device_register(KERNEL_DEVICE_TYPE_PCI, &controller, sizeof(controller)))
			return false;
	}
	return true;
}
