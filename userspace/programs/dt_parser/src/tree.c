#include "tree.h"

#include <protocol/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system/device_tree.h>

#define DT_COMPATIBLE_PREFIX "dt:"
#define DT_COMPATIBLE_PREFIX_SIZE (sizeof(DT_COMPATIBLE_PREFIX) - 1u)

static uint32_t read_u32_be(const uint8_t value[4]) {
	return ((uint32_t)value[0] << 24u) | ((uint32_t)value[1] << 16u) | ((uint32_t)value[2] << 8u) | (uint32_t)value[3];
}

uint32_t dt_parser_read_u32_le(const uint8_t value[4]) {
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8u) | ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
}

void dt_parser_write_u32_le(uint8_t value[4], uint32_t input) {
	for (size_t index = 0u; index < 4u; index++) value[index] = (uint8_t)(input >> (index * 8u));
}

void dt_parser_write_u64_le(uint8_t value[8], uint64_t input) {
	for (size_t index = 0u; index < 8u; index++) value[index] = (uint8_t)(input >> (index * 8u));
}

bool dt_parser_utf8_valid(const uint8_t* value, size_t size) {
	size_t index = 0u;

	if (value == NULL && size != 0u) return false;
	while (index < size) {
		uint8_t first = value[index++];
		if (first == 0u) return false;
		if (first <= 0x7fu) continue;
		if (first >= 0xc2u && first <= 0xdfu) {
			if (index >= size || value[index] < 0x80u || value[index] > 0xbfu) return false;
			index++;
			continue;
		}
		if (first >= 0xe0u && first <= 0xefu) {
			uint8_t second;
			if (size - index < 2u) return false;
			second = value[index];
			if (second < 0x80u || second > 0xbfu || value[index + 1u] < 0x80u || value[index + 1u] > 0xbfu ||
			    (first == 0xe0u && second < 0xa0u) || (first == 0xedu && second > 0x9fu))
				return false;
			index += 2u;
			continue;
		}
		if (first >= 0xf0u && first <= 0xf4u) {
			uint8_t second;
			if (size - index < 3u) return false;
			second = value[index];
			if (second < 0x80u || second > 0xbfu || value[index + 1u] < 0x80u || value[index + 1u] > 0xbfu ||
			    value[index + 2u] < 0x80u || value[index + 2u] > 0xbfu || (first == 0xf0u && second < 0x90u) ||
			    (first == 0xf4u && second > 0x8fu))
				return false;
			index += 3u;
			continue;
		}
		return false;
	}
	return true;
}

syscall_status_t dt_parser_walk_next(cap_id_t provider_cap, device_tree_node_id_t node,
                                     device_tree_node_id_t* out_next) {
	struct device_tree_node_info_response info;
	syscall_status_t                      status;

	if (out_next == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_next = DEVICE_TREE_NODE_INVALID;
	status    = device_tree_node_info(provider_cap, node, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.first_child != DEVICE_TREE_NODE_INVALID) {
		*out_next = info.first_child;
		return SYSCALL_STATUS_OK;
	}
	for (;;) {
		if (info.next_sibling != DEVICE_TREE_NODE_INVALID) {
			*out_next = info.next_sibling;
			return SYSCALL_STATUS_OK;
		}
		if (info.parent == DEVICE_TREE_NODE_INVALID) return SYSCALL_STATUS_OK;
		status = device_tree_node_info(provider_cap, info.parent, &info);
		if (status != SYSCALL_STATUS_OK) return status;
	}
}

syscall_status_t dt_parser_property_find(cap_id_t provider_cap, device_tree_node_id_t node, const char* name,
                                         struct dt_parser_property* out_property) {
	struct device_tree_node_info_response info;
	size_t                                name_size;
	syscall_status_t                      status;

	if (name == NULL || out_property == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	name_size = strlen(name);
	status    = device_tree_node_info(provider_cap, node, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	for (uint64_t index = 0u; index < info.property_count; index++) {
		struct device_tree_property_info_response property_info;
		char                                      candidate[DEVICE_IDENTIFIER_MAX + 1u];

		status = device_tree_property_info(provider_cap, node, index, &property_info);
		if (status != SYSCALL_STATUS_OK) return status;
		if (property_info.name_size != name_size) continue;
		if (name_size > DEVICE_IDENTIFIER_MAX) return SYSCALL_STATUS_UNAVAILABLE;
		status = device_tree_property_name_read(provider_cap, node, index, 0u, candidate, name_size);
		if (status != SYSCALL_STATUS_OK) return status;
		if (memcmp(candidate, name, name_size) != 0) continue;
		*out_property = (struct dt_parser_property){.index = index, .value_size = property_info.value_size};
		return SYSCALL_STATUS_OK;
	}
	return SYSCALL_STATUS_UNAVAILABLE;
}

syscall_status_t dt_parser_property_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                         const struct dt_parser_property* property, uint64_t offset, void* buffer,
                                         size_t size) {
	uint8_t* target  = buffer;
	size_t   written = 0u;

	if (property == NULL || (size != 0u && buffer == NULL) || offset > property->value_size ||
	    size > property->value_size - offset)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	while (written < size) {
		size_t chunk = size - written;
		if (chunk > CAP_MAX_RESPONSE_SIZE) chunk = CAP_MAX_RESPONSE_SIZE;
		syscall_status_t status =
			device_tree_property_read(provider_cap, node, property->index, offset + written, target + written, chunk);
		if (status != SYSCALL_STATUS_OK) return status;
		written += chunk;
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t property_cells(cap_id_t provider_cap, device_tree_node_id_t node,
                                       const struct dt_parser_property* property, size_t first_cell,
                                       uint32_t cell_count, uint64_t* out_value) {
	uint8_t  bytes[8];
	uint64_t offset;

	if (property == NULL || out_value == NULL || cell_count > 2u || first_cell > UINT64_MAX / 4u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	offset = (uint64_t)first_cell * 4u;
	if ((uint64_t)cell_count * 4u > property->value_size || offset > property->value_size - (uint64_t)cell_count * 4u)
		return SYSCALL_STATUS_UNAVAILABLE;
	syscall_status_t status = dt_parser_property_read(provider_cap, node, property, offset, bytes, cell_count * 4u);
	if (status != SYSCALL_STATUS_OK) return status;
	*out_value = cell_count == 0u ? 0u : read_u32_be(bytes);
	if (cell_count == 2u) *out_value = (*out_value << 32u) | read_u32_be(bytes + 4u);
	return SYSCALL_STATUS_OK;
}

static syscall_status_t property_u32(cap_id_t provider_cap, device_tree_node_id_t node, const char* name,
                                     uint32_t* out_value) {
	struct dt_parser_property property;
	uint8_t                   value[4];
	syscall_status_t          status;

	if (out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = dt_parser_property_find(provider_cap, node, name, &property);
	if (status != SYSCALL_STATUS_OK) return status;
	if (property.value_size != sizeof(value)) return SYSCALL_STATUS_UNAVAILABLE;
	status = dt_parser_property_read(provider_cap, node, &property, 0u, value, sizeof(value));
	if (status == SYSCALL_STATUS_OK) *out_value = read_u32_be(value);
	return status;
}

static syscall_status_t node_self_enabled(cap_id_t provider_cap, device_tree_node_id_t node, bool* out_enabled) {
	struct dt_parser_property property;
	char                      value[5];
	syscall_status_t          status;

	*out_enabled = false;
	status       = dt_parser_property_find(provider_cap, node, "status", &property);
	if (status == SYSCALL_STATUS_UNAVAILABLE) {
		*out_enabled = true;
		return SYSCALL_STATUS_OK;
	}
	if (status != SYSCALL_STATUS_OK) return status;
	if (property.value_size != sizeof("ok") && property.value_size != sizeof("okay")) return SYSCALL_STATUS_OK;
	status = dt_parser_property_read(provider_cap, node, &property, 0u, value, (size_t)property.value_size);
	if (status == SYSCALL_STATUS_OK) {
		if (property.value_size == sizeof("ok")) *out_enabled = memcmp(value, "ok", sizeof("ok")) == 0;
		else *out_enabled = memcmp(value, "okay", sizeof("okay")) == 0;
	}
	return status;
}

syscall_status_t dt_parser_node_enabled(cap_id_t provider_cap, device_tree_node_id_t node, bool* out_enabled) {
	if (out_enabled == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_enabled = false;
	while (node != DEVICE_TREE_NODE_INVALID) {
		struct device_tree_node_info_response info;
		bool                                  enabled;
		syscall_status_t                      status = node_self_enabled(provider_cap, node, &enabled);
		if (status != SYSCALL_STATUS_OK) return status;
		if (!enabled) return SYSCALL_STATUS_OK;
		status = device_tree_node_info(provider_cap, node, &info);
		if (status != SYSCALL_STATUS_OK) return status;
		node = info.parent;
	}
	*out_enabled = true;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t node_cells(cap_id_t provider_cap, device_tree_node_id_t node, const char* name,
                                   uint32_t fallback, bool zero_valid, uint32_t* out_value) {
	syscall_status_t status = property_u32(provider_cap, node, name, out_value);

	if (status == SYSCALL_STATUS_UNAVAILABLE) {
		*out_value = fallback;
		return SYSCALL_STATUS_OK;
	}
	if (status != SYSCALL_STATUS_OK) return status;
	return *out_value <= 2u && (zero_valid || *out_value != 0u) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_UNAVAILABLE;
}

static syscall_status_t translate_address(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t* address) {
	struct device_tree_node_info_response node_info;
	device_tree_node_id_t                 bus;
	syscall_status_t                      status;

	status = device_tree_node_info(provider_cap, node, &node_info);
	if (status != SYSCALL_STATUS_OK) return status;
	bus = node_info.parent;
	if (bus == DEVICE_TREE_NODE_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	for (;;) {
		struct device_tree_node_info_response bus_info;
		struct dt_parser_property             ranges;
		uint32_t                              child_cells;
		uint32_t                              parent_cells;
		uint32_t                              size_cells;
		size_t                                tuple_cells;
		size_t                                tuple_count;
		bool                                  matched    = false;
		uint64_t                              translated = 0u;

		status = device_tree_node_info(provider_cap, bus, &bus_info);
		if (status != SYSCALL_STATUS_OK) return status;
		if (bus_info.parent == DEVICE_TREE_NODE_INVALID) return SYSCALL_STATUS_OK;
		status = dt_parser_property_find(provider_cap, bus, "ranges", &ranges);
		if (status != SYSCALL_STATUS_OK) return status;
		if (ranges.value_size == 0u) {
			bus = bus_info.parent;
			continue;
		}
		status = node_cells(provider_cap, bus, "#address-cells", 2u, false, &child_cells);
		if (status != SYSCALL_STATUS_OK) return status;
		status = node_cells(provider_cap, bus_info.parent, "#address-cells", 2u, false, &parent_cells);
		if (status != SYSCALL_STATUS_OK) return status;
		status = node_cells(provider_cap, bus, "#size-cells", 1u, false, &size_cells);
		if (status != SYSCALL_STATUS_OK) return status;
		tuple_cells = (size_t)child_cells + parent_cells + size_cells;
		if (tuple_cells == 0u || tuple_cells > SIZE_MAX / 4u || ranges.value_size % (tuple_cells * 4u) != 0u)
			return SYSCALL_STATUS_UNAVAILABLE;
		tuple_count = (size_t)(ranges.value_size / (tuple_cells * 4u));
		for (size_t index = 0u; index < tuple_count; index++) {
			uint64_t child;
			uint64_t parent_address;
			uint64_t length;
			uint64_t end;
			size_t   first = index * tuple_cells;

			status = property_cells(provider_cap, bus, &ranges, first, child_cells, &child);
			if (status != SYSCALL_STATUS_OK) return status;
			status = property_cells(provider_cap, bus, &ranges, first + child_cells, parent_cells, &parent_address);
			if (status != SYSCALL_STATUS_OK) return status;
			status =
				property_cells(provider_cap, bus, &ranges, first + child_cells + parent_cells, size_cells, &length);
			if (status != SYSCALL_STATUS_OK) return status;
			if (length > UINT64_MAX - child) return SYSCALL_STATUS_UNAVAILABLE;
			end = child + length;
			if (*address < child || *address >= end) continue;
			if (parent_address > UINT64_MAX - (*address - child) || matched) return SYSCALL_STATUS_UNAVAILABLE;
			translated = parent_address + (*address - child);
			matched    = true;
		}
		if (!matched) return SYSCALL_STATUS_UNAVAILABLE;
		*address = translated;
		bus      = bus_info.parent;
	}
}

syscall_status_t dt_parser_ranges_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                       struct dt_parser_ranges* out_ranges) {
	struct device_tree_node_info_response node_info;
	struct dt_parser_property             property;
	struct dt_parser_range*               ranges;
	uint32_t                              address_cells;
	uint32_t                              size_cells;
	size_t                                tuple_cells;
	size_t                                tuple_count;
	syscall_status_t                      status;

	if (out_ranges == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_ranges = (struct dt_parser_ranges){0};
	status      = device_tree_node_info(provider_cap, node, &node_info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (node_info.parent == DEVICE_TREE_NODE_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	status = node_cells(provider_cap, node_info.parent, "#address-cells", 2u, false, &address_cells);
	if (status != SYSCALL_STATUS_OK) return status;
	status = node_cells(provider_cap, node_info.parent, "#size-cells", 1u, true, &size_cells);
	if (status != SYSCALL_STATUS_OK || size_cells == 0u)
		return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_UNAVAILABLE : status;
	status = dt_parser_property_find(provider_cap, node, "reg", &property);
	if (status != SYSCALL_STATUS_OK) return status;
	tuple_cells = (size_t)address_cells + size_cells;
	if (tuple_cells == 0u || tuple_cells > SIZE_MAX / 4u || property.value_size == 0u ||
	    property.value_size % (tuple_cells * 4u) != 0u)
		return SYSCALL_STATUS_UNAVAILABLE;
	tuple_count = (size_t)(property.value_size / (tuple_cells * 4u));
	if (tuple_count == 0u || tuple_count > DEVICE_MAX_STATE_SIZE / sizeof(*ranges)) return SYSCALL_STATUS_UNAVAILABLE;
	ranges = calloc(tuple_count, sizeof(*ranges));
	if (ranges == NULL) return SYSCALL_STATUS_FAILED;
	for (size_t index = 0u; index < tuple_count; index++) {
		size_t first = index * tuple_cells;

		status = property_cells(provider_cap, node, &property, first, address_cells, &ranges[index].base);
		if (status != SYSCALL_STATUS_OK) goto fail;
		status =
			property_cells(provider_cap, node, &property, first + address_cells, size_cells, &ranges[index].length);
		if (status != SYSCALL_STATUS_OK) goto fail;
		if (ranges[index].length == 0u || ranges[index].base > UINT64_MAX - ranges[index].length) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail;
		}
		status = translate_address(provider_cap, node, &ranges[index].base);
		if (status != SYSCALL_STATUS_OK) goto fail;
		if (ranges[index].base > UINT64_MAX - ranges[index].length) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail;
		}
	}
	out_ranges->values = ranges;
	out_ranges->count  = tuple_count;
	return SYSCALL_STATUS_OK;

fail:
	free(ranges);
	return status;
}

void dt_parser_ranges_deinit(struct dt_parser_ranges* ranges) {
	if (ranges == NULL) return;
	free(ranges->values);
	*ranges = (struct dt_parser_ranges){0};
}

static bool compatible_duplicate(const uint8_t* raw, size_t current_offset, const uint8_t* value, size_t value_size) {
	size_t offset = 0u;

	while (offset < current_offset) {
		size_t length = strlen((const char*)raw + offset);
		if (length == value_size && memcmp(raw + offset, value, value_size) == 0) return true;
		offset += length + 1u;
	}
	return false;
}

syscall_status_t dt_parser_compatibles_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                            struct dt_parser_string_list* out_compatibles) {
	struct dt_parser_property property;
	uint8_t*                  raw;
	uint8_t*                  encoded;
	size_t                    count = 0u;
	size_t                    offset;
	size_t                    encoded_size;
	size_t                    encoded_offset;
	syscall_status_t          status;

	if (out_compatibles == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_compatibles = (struct dt_parser_string_list){0};
	status           = dt_parser_property_find(provider_cap, node, "compatible", &property);
	if (status != SYSCALL_STATUS_OK) return status;
	if (property.value_size == 0u || property.value_size > SIZE_MAX || property.value_size > DEVICE_MAX_STATE_SIZE)
		return SYSCALL_STATUS_UNAVAILABLE;
	raw = malloc((size_t)property.value_size);
	if (raw == NULL) return SYSCALL_STATUS_FAILED;
	status = dt_parser_property_read(provider_cap, node, &property, 0u, raw, (size_t)property.value_size);
	if (status != SYSCALL_STATUS_OK) goto fail_raw;
	offset = 0u;
	while (offset < property.value_size) {
		uint8_t* end = memchr(raw + offset, 0, (size_t)property.value_size - offset);
		size_t   length;
		if (end == NULL) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail_raw;
		}
		length = (size_t)(end - (raw + offset));
		if (length == 0u || length > DEVICE_COMPATIBLE_ID_MAX - DT_COMPATIBLE_PREFIX_SIZE ||
		    !dt_parser_utf8_valid(raw + offset, length) || compatible_duplicate(raw, offset, raw + offset, length) ||
		    count == DEVICE_MAX_COMPATIBLE_IDS) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail_raw;
		}
		count++;
		offset += length + 1u;
	}
	if (count > (SIZE_MAX - (size_t)property.value_size) / (3u + DT_COMPATIBLE_PREFIX_SIZE)) {
		status = SYSCALL_STATUS_UNAVAILABLE;
		goto fail_raw;
	}
	encoded_size = (size_t)property.value_size + count * (3u + DT_COMPATIBLE_PREFIX_SIZE);
	encoded      = malloc(encoded_size);
	if (encoded == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto fail_raw;
	}
	offset         = 0u;
	encoded_offset = 0u;
	while (offset < property.value_size) {
		size_t length = strlen((const char*)raw + offset);
		dt_parser_write_u32_le(encoded + encoded_offset, (uint32_t)(length + DT_COMPATIBLE_PREFIX_SIZE));
		encoded_offset += sizeof(uint32_t);
		memcpy(encoded + encoded_offset, DT_COMPATIBLE_PREFIX, DT_COMPATIBLE_PREFIX_SIZE);
		encoded_offset += DT_COMPATIBLE_PREFIX_SIZE;
		memcpy(encoded + encoded_offset, raw + offset, length);
		encoded_offset += length;
		offset += length + 1u;
	}
	free(raw);
	out_compatibles->value      = encoded;
	out_compatibles->value_size = encoded_size;
	out_compatibles->count      = count;
	return SYSCALL_STATUS_OK;

fail_raw:
	free(raw);
	return status;
}

void dt_parser_string_list_deinit(struct dt_parser_string_list* list) {
	if (list == NULL) return;
	free(list->value);
	*list = (struct dt_parser_string_list){0};
}
