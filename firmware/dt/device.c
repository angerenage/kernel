#include <base/math.h>
#include <firmware/dt/device.h>
#include <stddef.h>
#include <stdint.h>

static struct dt_node dt_walk_next(struct dt_node node) {
	struct dt_node next;

	if (!dt_node_valid(node)) return DT_NODE_INVALID;
	next = dt_node_child(node);
	if (dt_node_valid(next)) return next;
	while (dt_node_valid(node)) {
		next = dt_node_next(node);
		if (dt_node_valid(next)) return next;
		node = dt_node_parent(node);
	}
	return DT_NODE_INVALID;
}

size_t dt_device_count(const char* compatible) {
	struct dt_node node  = dt_root();
	size_t         count = 0u;

	if (compatible == NULL) return 0u;
	while (dt_node_valid(node)) {
		if (dt_node_enabled(node) && dt_node_compatible(node, compatible)) count++;
		node = dt_walk_next(node);
	}
	return count;
}

struct dt_node dt_device_at(const char* compatible, size_t index) {
	struct dt_node node    = dt_root();
	size_t         current = 0u;

	if (compatible == NULL) return DT_NODE_INVALID;
	while (dt_node_valid(node)) {
		if (dt_node_enabled(node) && dt_node_compatible(node, compatible)) {
			if (current == index) return node;
			current++;
		}
		node = dt_walk_next(node);
	}
	return DT_NODE_INVALID;
}

struct dt_node dt_node_with_string_at(const char* name, const char* value, size_t index) {
	struct dt_node node    = dt_root();
	size_t         current = 0u;

	if (name == NULL || value == NULL) return DT_NODE_INVALID;
	while (dt_node_valid(node)) {
		struct dt_property property;

		if (dt_node_property(node, name, &property) && dt_property_string_list_contains(&property, value)) {
			if (current == index) return node;
			current++;
		}
		node = dt_walk_next(node);
	}
	return DT_NODE_INVALID;
}

static bool dt_node_cells(struct dt_node node, const char* name, uint32_t fallback, bool zero_valid, uint32_t* out) {
	struct dt_property property;
	uint64_t           value;

	if (!dt_node_valid(node) || name == NULL || out == NULL) return false;
	if (!dt_node_property(node, name, &property)) {
		*out = fallback;
		return true;
	}
	if (property.size != 4u || !dt_property_read_cells(&property, 0u, 1u, &value) || value > 2u ||
	    (!zero_valid && value == 0u))
		return false;
	*out = (uint32_t)value;
	return true;
}

bool dt_node_reg_raw(struct dt_node node, size_t index, struct dt_reg* out) {
	struct dt_node     parent;
	struct dt_property property;
	struct dt_reg      reg;
	uint32_t           address_cells;
	uint32_t           size_cells;
	size_t             tuple_cells;
	size_t             tuple_size;
	size_t             first_cell;

	if (out == NULL || !dt_node_valid(node)) return false;
	parent = dt_node_parent(node);
	if (!dt_node_valid(parent) || !dt_node_cells(parent, "#address-cells", 2u, false, &address_cells) ||
	    !dt_node_cells(parent, "#size-cells", 1u, true, &size_cells) || !dt_node_property(node, "reg", &property))
		return false;
	tuple_cells = (size_t)address_cells + size_cells;
	if (tuple_cells == 0u || tuple_cells > SIZE_MAX / 4u) return false;
	tuple_size = tuple_cells * 4u;
	if (property.size % tuple_size != 0u || index >= property.size / tuple_size || index > SIZE_MAX / tuple_cells)
		return false;
	first_cell = index * tuple_cells;
	if (!dt_property_read_cells(&property, first_cell, address_cells, &reg.address)) return false;
	reg.size = 0u;
	if (size_cells != 0u && !dt_property_read_cells(&property, first_cell + address_cells, size_cells, &reg.size))
		return false;
	*out = reg;
	return true;
}

static bool dt_node_translate(struct dt_node node, uint64_t* address) {
	struct dt_node bus = dt_node_parent(node);

	if (address == NULL || !dt_node_valid(bus)) return false;
	for (;;) {
		struct dt_node parent = dt_node_parent(bus);

		if (!dt_node_valid(parent)) return true;
		struct dt_property ranges;
		uint32_t           child_cells;
		uint32_t           parent_cells;
		uint32_t           size_cells;
		size_t             tuple_cells;
		size_t             tuple_size;
		bool               matched    = false;
		uint64_t           translated = 0u;

		if (!dt_node_property(bus, "ranges", &ranges)) return false;
		if (ranges.size == 0u) {
			bus = parent;
			continue;
		}
		if (!dt_node_cells(bus, "#address-cells", 2u, false, &child_cells) ||
		    !dt_node_cells(parent, "#address-cells", 2u, false, &parent_cells) ||
		    !dt_node_cells(bus, "#size-cells", 1u, false, &size_cells))
			return false;
		tuple_cells = (size_t)child_cells + parent_cells + size_cells;
		if (tuple_cells == 0u || tuple_cells > SIZE_MAX / 4u) return false;
		tuple_size = tuple_cells * 4u;
		if (ranges.size % tuple_size != 0u) return false;
		for (size_t first_cell = 0u; first_cell < ranges.size / 4u; first_cell += tuple_cells) {
			uint64_t child;
			uint64_t parent_address;
			uint64_t size;
			uint64_t end;
			uint64_t candidate;

			if (!dt_property_read_cells(&ranges, first_cell, child_cells, &child) ||
			    !dt_property_read_cells(&ranges, first_cell + child_cells, parent_cells, &parent_address) ||
			    !dt_property_read_cells(&ranges, first_cell + child_cells + parent_cells, size_cells, &size) ||
			    add_overflow_u64(child, size, &end))
				return false;
			if (*address < child || *address >= end) continue;
			if (add_overflow_u64(parent_address, *address - child, &candidate) || matched) return false;
			translated = candidate;
			matched    = true;
		}
		if (!matched) return false;
		*address = translated;
		bus      = parent;
	}
}

bool dt_node_reg(struct dt_node node, size_t index, struct dt_reg* out) {
	struct dt_reg reg;

	if (out == NULL || !dt_node_reg_raw(node, index, &reg) || !dt_node_translate(node, &reg.address)) return false;
	*out = reg;
	return true;
}

static bool dt_node_u32(struct dt_node node, const char* name, uint32_t* out_value) {
	struct dt_property property;
	uint64_t           value;

	if (out_value == NULL || !dt_node_property(node, name, &property) || property.size != sizeof(uint32_t) ||
	    !dt_property_read_cells(&property, 0u, 1u, &value) || value > UINT32_MAX)
		return false;
	*out_value = (uint32_t)value;
	return true;
}

bool dt_pci_iommu_route(uint16_t segment, uint16_t requester_id, uint64_t* out_controller_address,
                        uint32_t* out_source_id) {
	struct dt_node host    = dt_root();
	bool           matched = false;

	if (out_controller_address == NULL || out_source_id == NULL) return false;
	while (dt_node_valid(host)) {
		struct dt_property map;
		struct dt_property mask_property;
		struct dt_property device_type;
		uint32_t           domain = 0u;
		uint32_t           mask   = UINT32_MAX;

		if (!dt_node_enabled(host) || !dt_node_property(host, "device_type", &device_type) ||
		    !dt_property_string_list_contains(&device_type, "pci") || !dt_node_property(host, "iommu-map", &map)) {
			host = dt_walk_next(host);
			continue;
		}
		(void)dt_node_u32(host, "linux,pci-domain", &domain);
		if (domain != segment || map.size == 0u || map.size % sizeof(uint32_t) != 0u) {
			host = dt_walk_next(host);
			continue;
		}
		if (dt_node_property(host, "iommu-map-mask", &mask_property)) {
			uint64_t value;
			if (mask_property.size != sizeof(uint32_t) || !dt_property_read_cells(&mask_property, 0u, 1u, &value) ||
			    value > UINT32_MAX)
				return false;
			mask = (uint32_t)value;
		}
		uint32_t routed_id = requester_id & mask;
		for (size_t cell = 0u; cell < map.size / sizeof(uint32_t);) {
			uint64_t       rid_base;
			uint64_t       phandle;
			uint64_t       source_base;
			uint64_t       length;
			struct dt_node controller;
			uint32_t       iommu_cells;
			struct dt_reg  registers;

			if (cell + 2u > map.size / sizeof(uint32_t) || !dt_property_read_cells(&map, cell, 1u, &rid_base) ||
			    !dt_property_read_cells(&map, cell + 1u, 1u, &phandle) || phandle == 0u || phandle > UINT32_MAX)
				return false;
			controller = dt_node_by_phandle((uint32_t)phandle);
			if (!dt_node_valid(controller) || !dt_node_u32(controller, "#iommu-cells", &iommu_cells) ||
			    iommu_cells != 1u || cell + 3u + iommu_cells > map.size / sizeof(uint32_t) ||
			    !dt_property_read_cells(&map, cell + 2u, iommu_cells, &source_base) ||
			    !dt_property_read_cells(&map, cell + 2u + iommu_cells, 1u, &length) || length == 0u ||
			    rid_base > UINT32_MAX || source_base > UINT32_MAX || length > UINT32_MAX)
				return false;
			if (routed_id >= rid_base && routed_id - rid_base < length) {
				uint64_t source = source_base + (routed_id - rid_base);
				if (source > UINT32_MAX || !dt_node_reg(controller, 0u, &registers) || registers.address == 0u ||
				    matched)
					return false;
				*out_controller_address = registers.address;
				*out_source_id          = (uint32_t)source;
				matched                 = true;
			}
			cell += 3u + iommu_cells;
		}
		host = dt_walk_next(host);
	}
	return matched;
}
