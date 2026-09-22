#pragma once

#include <base/math.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IOMMU_FDT_MAGIC 0xd00dfeedu
#define IOMMU_FDT_BEGIN_NODE 1u
#define IOMMU_FDT_END_NODE 2u
#define IOMMU_FDT_PROPERTY 3u
#define IOMMU_FDT_NOP 4u
#define IOMMU_FDT_END 9u
#define IOMMU_FDT_MAX_DEPTH 32u

struct iommu_fdt_node {
	uint32_t       parent_address_cells;
	uint32_t       parent_size_cells;
	uint32_t       address_cells;
	uint32_t       size_cells;
	const uint8_t* compatible;
	size_t         compatible_size;
	const uint8_t* status;
	size_t         status_size;
	const uint8_t* reg;
	size_t         reg_size;
	const uint8_t* ranges;
	size_t         ranges_size;
	const uint8_t* requested_property;
	size_t         requested_property_size;
	bool           requested_property_present;
};

static inline uint32_t iommu_fdt_u32(const void* data) {
	const uint8_t* bytes = data;
	return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static inline bool iommu_fdt_cells(const uint8_t* data, uint32_t cells, uint64_t* out) {
	if (data == NULL || out == NULL || cells == 0u || cells > 2u) return false;
	uint64_t value = 0u;
	for (uint32_t index = 0u; index < cells; index++) value = (value << 32u) | iommu_fdt_u32(data + index * 4u);
	*out = value;
	return true;
}

static inline bool iommu_fdt_string_list_contains(const uint8_t* data, size_t size, const char* wanted) {
	if (data == NULL || wanted == NULL) return false;
	while (size != 0u) {
		size_t length = 0u;
		while (length < size && data[length] != '\0') length++;
		if (length == size) return false;
		if (strcmp((const char*)data, wanted) == 0) return true;
		data += length + 1u;
		size -= length + 1u;
	}
	return false;
}

static inline bool iommu_fdt_node_enabled(const struct iommu_fdt_node* node) {
	return node->status == NULL || (node->status_size == 3u && memcmp(node->status, "ok\0", 3u) == 0) ||
	       (node->status_size == 5u && memcmp(node->status, "okay\0", 5u) == 0);
}

static inline bool iommu_fdt_translate(const struct iommu_fdt_node* stack, size_t depth, uint64_t* address) {
	if (address == NULL || depth == 0u) return false;
	size_t level = depth - 1u;
	while (level > 1u) {
		level--;
		const struct iommu_fdt_node* bus = &stack[level];
		if (bus->ranges == NULL) return false;
		if (bus->ranges_size == 0u) continue;
		size_t tuple_cells = (size_t)bus->address_cells + bus->parent_address_cells + bus->size_cells;
		if (tuple_cells == 0u || tuple_cells > SIZE_MAX / 4u || bus->ranges_size % (tuple_cells * 4u) != 0u)
			return false;
		bool translated = false;
		for (size_t offset = 0u; offset < bus->ranges_size; offset += tuple_cells * 4u) {
			uint64_t       child;
			uint64_t       parent;
			uint64_t       size;
			const uint8_t* tuple = bus->ranges + offset;
			if (!iommu_fdt_cells(tuple, bus->address_cells, &child) ||
			    !iommu_fdt_cells(tuple + bus->address_cells * 4u, bus->parent_address_cells, &parent) ||
			    !iommu_fdt_cells(tuple + (bus->address_cells + bus->parent_address_cells) * 4u, bus->size_cells, &size))
				return false;
			uint64_t child_end;
			if (add_overflow_u64(child, size, &child_end) || *address < child || *address >= child_end) continue;
			if (add_overflow_u64(parent, *address - child, address)) return false;
			translated = true;
			break;
		}
		if (!translated) return false;
	}
	return true;
}

static inline bool iommu_fdt_node_reg_address(const struct iommu_fdt_node* stack, size_t depth, size_t ordinal,
                                              uint64_t* out) {
	const struct iommu_fdt_node* node       = &stack[depth - 1u];
	size_t                       tuple_size = (size_t)(node->parent_address_cells + node->parent_size_cells) * 4u;
	if (node->reg == NULL || node->parent_address_cells == 0u || node->parent_address_cells > 2u || tuple_size == 0u ||
	    node->reg_size % tuple_size != 0u || ordinal >= node->reg_size / tuple_size ||
	    !iommu_fdt_cells(node->reg + ordinal * tuple_size, node->parent_address_cells, out))
		return false;
	return iommu_fdt_translate(stack, depth, out);
}

static inline bool iommu_fdt_node_reg_size(const struct iommu_fdt_node* stack, size_t depth, size_t ordinal,
                                           uint64_t* out) {
	const struct iommu_fdt_node* node       = &stack[depth - 1u];
	size_t                       tuple_size = (size_t)(node->parent_address_cells + node->parent_size_cells) * 4u;
	if (node->reg == NULL || out == NULL || node->parent_size_cells == 0u || node->parent_size_cells > 2u ||
	    tuple_size == 0u || node->reg_size % tuple_size != 0u || ordinal >= node->reg_size / tuple_size)
		return false;
	return iommu_fdt_cells(
		node->reg + ordinal * tuple_size + node->parent_address_cells * 4u, node->parent_size_cells, out);
}

static inline bool iommu_fdt_node_address(const struct iommu_fdt_node* stack, size_t depth, uint64_t* out) {
	return iommu_fdt_node_reg_address(stack, depth, 0u, out);
}

static inline size_t iommu_fdt_controllers_search(const char* compatible, size_t target, uintptr_t* out_address,
                                                  uintptr_t* out_first_size, uintptr_t* out_second_address,
                                                  uintptr_t* out_second_size, bool* out_present,
                                                  const char* property_name, const uint8_t** out_property,
                                                  size_t* out_property_size, bool* out_property_present) {
	struct kernel_boot_data dtb;
	struct iommu_fdt_node   stack[IOMMU_FDT_MAX_DEPTH];
	if (!kernel_boot_dtb_get(&dtb) || dtb.address == NULL || dtb.size < 40u ||
	    iommu_fdt_u32(dtb.address) != IOMMU_FDT_MAGIC)
		return 0u;
	const uint8_t* blob             = dtb.address;
	uint32_t       total_size       = iommu_fdt_u32(blob + 4u);
	uint32_t       structure_offset = iommu_fdt_u32(blob + 8u);
	uint32_t       strings_offset   = iommu_fdt_u32(blob + 12u);
	uint32_t       strings_size     = iommu_fdt_u32(blob + 32u);
	uint32_t       structure_size   = iommu_fdt_u32(blob + 36u);
	if (total_size < 40u || total_size > dtb.size || structure_offset > total_size ||
	    structure_size > total_size - structure_offset || strings_offset > total_size ||
	    strings_size > total_size - strings_offset)
		return 0u;
	const uint8_t* cursor  = blob + structure_offset;
	const uint8_t* end     = cursor + structure_size;
	const uint8_t* strings = blob + strings_offset;
	size_t         depth   = 0u;
	size_t         count   = 0u;
	while ((size_t)(end - cursor) >= 4u) {
		uint32_t token = iommu_fdt_u32(cursor);
		cursor += 4u;
		if (token == IOMMU_FDT_BEGIN_NODE) {
			if (depth == IOMMU_FDT_MAX_DEPTH) return count;
			const uint8_t* name_end = memchr(cursor, '\0', (size_t)(end - cursor));
			if (name_end == NULL) return count;
			uint32_t parent_address_cells = depth == 0u ? 2u : stack[depth - 1u].address_cells;
			uint32_t parent_size_cells    = depth == 0u ? 1u : stack[depth - 1u].size_cells;
			stack[depth++]                = (struct iommu_fdt_node){.parent_address_cells = parent_address_cells,
			                                                        .parent_size_cells    = parent_size_cells,
			                                                        .address_cells        = 2u,
			                                                        .size_cells           = 1u};
			size_t name_size              = (size_t)(name_end - cursor) + 1u;
			cursor += (name_size + 3u) & ~(size_t)3u;
		}
		else if (token == IOMMU_FDT_END_NODE) {
			if (depth == 0u) return count;
			struct iommu_fdt_node* node = &stack[depth - 1u];
			if (iommu_fdt_node_enabled(node) &&
			    iommu_fdt_string_list_contains(node->compatible, node->compatible_size, compatible)) {
				if (out_present != NULL) *out_present = true;
				uint64_t address;
				if (iommu_fdt_node_address(stack, depth, &address) && address <= UINTPTR_MAX) {
					if (count == target) {
						if (out_address != NULL) *out_address = (uintptr_t)address;
						if (out_first_size != NULL) {
							uint64_t first_size;
							if (!iommu_fdt_node_reg_size(stack, depth, 0u, &first_size) || first_size > UINTPTR_MAX)
								return count;
							*out_first_size = (uintptr_t)first_size;
						}
						if (out_property != NULL) *out_property = node->requested_property;
						if (out_property_size != NULL) *out_property_size = node->requested_property_size;
						if (out_property_present != NULL) *out_property_present = node->requested_property_present;
						if (out_second_address != NULL || out_second_size != NULL) {
							uint64_t second;
							uint64_t second_size;
							if (!iommu_fdt_node_reg_address(stack, depth, 1u, &second) || second > UINTPTR_MAX ||
							    (out_second_size != NULL && (!iommu_fdt_node_reg_size(stack, depth, 1u, &second_size) ||
							                                 second_size > UINTPTR_MAX)))
								return count;
							if (out_second_address != NULL) *out_second_address = (uintptr_t)second;
							if (out_second_size != NULL) *out_second_size = (uintptr_t)second_size;
						}
					}
					count++;
				}
			}
			depth--;
		}
		else if (token == IOMMU_FDT_PROPERTY) {
			if (depth == 0u || (size_t)(end - cursor) < 8u) return count;
			uint32_t length      = iommu_fdt_u32(cursor);
			uint32_t name_offset = iommu_fdt_u32(cursor + 4u);
			cursor += 8u;
			if (length > (size_t)(end - cursor) || name_offset >= strings_size) return count;
			const char* name = (const char*)strings + name_offset;
			if (memchr(name, '\0', strings_size - name_offset) == NULL) return count;
			struct iommu_fdt_node* node = &stack[depth - 1u];
			if (strcmp(name, "#address-cells") == 0 && length == 4u) node->address_cells = iommu_fdt_u32(cursor);
			else if (strcmp(name, "#size-cells") == 0 && length == 4u) node->size_cells = iommu_fdt_u32(cursor);
			else if (strcmp(name, "compatible") == 0) {
				node->compatible      = cursor;
				node->compatible_size = length;
			}
			else if (strcmp(name, "status") == 0) {
				node->status      = cursor;
				node->status_size = length;
			}
			else if (strcmp(name, "reg") == 0) {
				node->reg      = cursor;
				node->reg_size = length;
			}
			else if (strcmp(name, "ranges") == 0) {
				node->ranges      = cursor;
				node->ranges_size = length;
			}
			if (property_name != NULL && strcmp(name, property_name) == 0) {
				node->requested_property         = cursor;
				node->requested_property_size    = length;
				node->requested_property_present = true;
			}
			cursor += (length + 3u) & ~3u;
		}
		else if (token == IOMMU_FDT_NOP) continue;
		else if (token == IOMMU_FDT_END) break;
		else return count;
		if (cursor > end) return count;
	}
	return count;
}

static inline size_t iommu_fdt_controllers_full(const char* compatible, size_t target, uintptr_t* out_address,
                                                uintptr_t* out_second_address) {
	return iommu_fdt_controllers_search(
		compatible, target, out_address, NULL, out_second_address, NULL, NULL, NULL, NULL, NULL, NULL);
}

static inline size_t iommu_fdt_controllers_second_region(const char* compatible, size_t target, uintptr_t* out_address,
                                                         uintptr_t* out_second_address, uintptr_t* out_second_size) {
	return iommu_fdt_controllers_search(
		compatible, target, out_address, NULL, out_second_address, out_second_size, NULL, NULL, NULL, NULL, NULL);
}

static inline size_t iommu_fdt_controllers_region(const char* compatible, size_t target, uintptr_t* out_address,
                                                  uintptr_t* out_size) {
	return iommu_fdt_controllers_search(
		compatible, target, out_address, out_size, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

static inline bool iommu_fdt_controller_property(const char* compatible, size_t target, const char* name,
                                                 const uint8_t** out_data, size_t* out_size) {
	bool present = false;
	if (iommu_fdt_controllers_search(
			compatible, target, NULL, NULL, NULL, NULL, NULL, name, out_data, out_size, &present) <= target)
		return false;
	return present;
}

static inline size_t iommu_fdt_controllers(const char* compatible, size_t target, uintptr_t* out_address) {
	return iommu_fdt_controllers_full(compatible, target, out_address, NULL);
}

static inline bool iommu_fdt_compatible_present(const char* compatible) {
	bool present = false;
	(void)iommu_fdt_controllers_search(compatible, 0u, NULL, NULL, NULL, NULL, &present, NULL, NULL, NULL, NULL);
	return present;
}
