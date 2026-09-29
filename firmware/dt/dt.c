#include <firmware/dt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DT_MAGIC 0xd00dfeedu
#define DT_BEGIN_NODE 1u
#define DT_END_NODE 2u
#define DT_PROPERTY 3u
#define DT_NOP 4u
#define DT_END 9u
#define DT_MIN_VERSION 2u
#define DT_MAX_VERSION 17u
#define DT_V1_HEADER_SIZE 28u
#define DT_V2_HEADER_SIZE 32u
#define DT_V3_HEADER_SIZE 36u
#define DT_V17_HEADER_SIZE 40u
#define DT_MAX_SIZE (16u * 1024u * 1024u)

struct dt_state {
	const uint8_t* blob;
	size_t         blob_size;
	const uint8_t* structure;
	size_t         structure_size;
	const uint8_t* strings;
	size_t         strings_size;
	uint32_t       version;
};

struct dt_token {
	uint32_t type;
	size_t   next;
	size_t   data;
	size_t   size;
	uint32_t name_offset;
};

static struct dt_state         dt;
static bool                    dt_initialized;
static const struct boot_info* dt_boot_info;

static uint32_t dt_read_u32(const uint8_t* value) {
	return ((uint32_t)value[0] << 24u) | ((uint32_t)value[1] << 16u) | ((uint32_t)value[2] << 8u) | (uint32_t)value[3];
}

static uint64_t dt_read_u64(const uint8_t* value) {
	return ((uint64_t)dt_read_u32(value) << 32u) | dt_read_u32(value + 4u);
}

static bool dt_align(size_t value, size_t alignment, size_t* out) {
	if (out == NULL || alignment == 0u || value > SIZE_MAX - (alignment - 1u)) return false;
	*out = (value + alignment - 1u) & ~(alignment - 1u);
	return true;
}

static bool dt_physical_span(uintptr_t physical, size_t size, const void** out) {
	const struct mem_range* ranges;
	size_t                  range_count;
	uintptr_t               end;
	uintptr_t               cursor;
	uintptr_t               virtual;

	if (size == 0u || size > UINTPTR_MAX - physical || dt_boot_info == NULL) return false;
	end         = physical + size;
	ranges      = dt_boot_info->memory_map;
	range_count = dt_boot_info->memory_map_count;
	if (ranges == NULL || range_count == 0u) return false;

	cursor = physical;
	while (cursor < end) {
		uintptr_t covered_end = cursor;

		for (size_t index = 0u; index < range_count; index++) {
			uintptr_t range_end;

			if (ranges[index].type != MEM_RANGE_BOOTLOADER_RECLAIMABLE || ranges[index].length == 0u ||
			    ranges[index].length > UINTPTR_MAX - ranges[index].base)
				continue;
			range_end = ranges[index].base + ranges[index].length;
			if (ranges[index].base <= cursor && range_end > covered_end) covered_end = range_end;
		}
		if (covered_end == cursor) return false;
		cursor = covered_end < end ? covered_end : end;
	}

	if (physical > UINTPTR_MAX - dt_boot_info->direct_map_offset) return false;
	virtual = physical + dt_boot_info->direct_map_offset;
	if (size > UINTPTR_MAX - virtual) return false;
	if (out != NULL) *out = (const void*)virtual;
	return true;
}

static bool dt_range(size_t offset, size_t size, size_t total) {
	return offset <= total && size <= total - offset;
}

static bool dt_overlap(size_t first, size_t first_size, size_t second, size_t second_size) {
	return first < second + second_size && second < first + first_size;
}

static bool dt_zeroes(const uint8_t* bytes, size_t size) {
	for (size_t index = 0u; index < size; index++)
		if (bytes[index] != 0u) return false;
	return true;
}

static size_t dt_header_size(uint32_t version) {
	if (version == 2u) return DT_V2_HEADER_SIZE;
	if (version < 17u) return DT_V3_HEADER_SIZE;
	return DT_V17_HEADER_SIZE;
}

static bool dt_token_read(const struct dt_state* state, size_t offset, size_t limit, struct dt_token* out) {
	struct dt_token token;
	size_t          cursor;

	if (state == NULL || out == NULL || offset > limit || limit > state->structure_size || limit - offset < 4u)
		return false;
	token  = (struct dt_token){.type = dt_read_u32(state->structure + offset)};
	cursor = offset + 4u;
	switch (token.type) {
	case DT_BEGIN_NODE: {
		const uint8_t* end = memchr(state->structure + cursor, '\0', limit - cursor);
		if (end == NULL || !dt_align((size_t)(end - state->structure) + 1u, 4u, &token.next) || token.next > limit)
			return false;
		token.data = cursor;
		token.size = (size_t)(end - (state->structure + cursor));
		if (!dt_zeroes(end + 1u, state->structure + token.next - (end + 1u))) return false;
		break;
	}
	case DT_PROPERTY: {
		size_t data;
		size_t end;

		if (limit - cursor < 8u) return false;
		token.size        = dt_read_u32(state->structure + cursor);
		token.name_offset = dt_read_u32(state->structure + cursor + 4u);
		data              = cursor + 8u;
		if (state->version < 16u && token.size >= 8u && (data & 7u) != 0u) {
			if (!dt_range(data, 4u, limit) || !dt_zeroes(state->structure + data, 4u)) return false;
			data += 4u;
		}
		if (!dt_range(data, token.size, limit) || !dt_align(data + token.size, 4u, &end) || end > limit) return false;
		token.data = data;
		token.next = end;
		break;
	}
	case DT_END_NODE:
	case DT_NOP:
	case DT_END:
		token.next = cursor;
		break;
	default:
		return false;
	}
	*out = token;
	return true;
}

static bool dt_structure_validate(struct dt_state* state, size_t limit, size_t* out_size) {
	size_t cursor      = 0u;
	size_t depth       = 0u;
	bool   root_seen   = false;
	bool   root_closed = false;

	while (cursor < limit) {
		struct dt_token token;

		if (!dt_token_read(state, cursor, limit, &token)) return false;
		switch (token.type) {
		case DT_BEGIN_NODE:
			if (root_closed) return false;
			if (depth == 0u) {
				if (root_seen || token.size != 0u) return false;
				root_seen = true;
			}
			depth++;
			break;
		case DT_END_NODE:
			if (depth == 0u) return false;
			depth--;
			if (depth == 0u) root_closed = true;
			break;
		case DT_PROPERTY:
			if (depth == 0u || token.name_offset >= state->strings_size ||
			    memchr(state->strings + token.name_offset, '\0', state->strings_size - token.name_offset) == NULL)
				return false;
			break;
		case DT_NOP:
			if (root_closed) return false;
			break;
		case DT_END:
			if (!root_seen || !root_closed || depth != 0u) return false;
			if (out_size != NULL) *out_size = token.next;
			return true;
		default:
			return false;
		}
		cursor = token.next;
	}
	return false;
}

static bool dt_reservations_validate(const uint8_t* blob, size_t total, size_t offset, size_t* out_size) {
	size_t cursor = offset;

	while (dt_range(cursor, 16u, total)) {
		uint64_t address = dt_read_u64(blob + cursor);
		uint64_t size    = dt_read_u64(blob + cursor + 8u);

		cursor += 16u;
		if (address == 0u && size == 0u) {
			if (out_size != NULL) *out_size = cursor - offset;
			return true;
		}
	}
	return false;
}

bool dt_init(const struct boot_info* info) {
	const uint8_t*  bytes;
	const void*     span;
	uintptr_t       virtual;
	uintptr_t       physical;
	uint32_t        total32;
	uint32_t        version;
	uint32_t        last_compatible;
	size_t          total;
	size_t          header_size;
	size_t          structure_offset;
	size_t          structure_limit;
	size_t          structure_declared;
	size_t          structure_size;
	size_t          strings_offset;
	size_t          strings_size;
	size_t          reservations_offset;
	size_t          reservations_size;
	struct dt_state state;

	const void* dtb;
	if (__atomic_load_n(&dt_initialized, __ATOMIC_ACQUIRE)) return true;
	if (info == NULL || info->dtb_address == 0u) return false;
	dt_boot_info = info;
	dtb          = (const void*)info->dtb_address;
	if (((uintptr_t)dtb & 7u) != 0u) return false;
	virtual = (uintptr_t)dtb;
	if (virtual < info->direct_map_offset) return false;
	physical = virtual - info->direct_map_offset;
	if (!dt_physical_span(physical, DT_V1_HEADER_SIZE, &span) || span != dtb) return false;
	bytes = span;
	if (dt_read_u32(bytes) != DT_MAGIC) return false;
	version         = dt_read_u32(bytes + 20u);
	last_compatible = dt_read_u32(bytes + 24u);
	if (version < DT_MIN_VERSION || version > DT_MAX_VERSION || last_compatible == 0u || last_compatible > version ||
	    last_compatible > DT_MAX_VERSION)
		return false;
	header_size = dt_header_size(version);
	if (!dt_physical_span(physical, header_size, &span) || span != dtb) return false;
	bytes   = span;
	total32 = dt_read_u32(bytes + 4u);
	total   = total32;
	if (total < header_size || total > DT_MAX_SIZE || !dt_physical_span(physical, total, &span) || span != dtb)
		return false;
	bytes = span;

	structure_offset    = dt_read_u32(bytes + 8u);
	strings_offset      = dt_read_u32(bytes + 12u);
	reservations_offset = dt_read_u32(bytes + 16u);
	if (structure_offset < header_size || (structure_offset & 3u) != 0u || strings_offset < header_size ||
	    reservations_offset < header_size || (reservations_offset & 7u) != 0u)
		return false;
	if (version >= 3u) {
		strings_size = dt_read_u32(bytes + 32u);
		if (!dt_range(strings_offset, strings_size, total)) return false;
	}
	else {
		if (strings_offset > total) return false;
		strings_size = total - strings_offset;
	}

	if (version >= 17u) {
		structure_declared = dt_read_u32(bytes + 36u);
		if (!dt_range(structure_offset, structure_declared, total)) return false;
		structure_limit = structure_declared;
	}
	else {
		if (structure_offset >= total) return false;
		structure_limit = total - structure_offset;
		if (strings_offset > structure_offset && strings_offset - structure_offset < structure_limit)
			structure_limit = strings_offset - structure_offset;
		if (reservations_offset > structure_offset && reservations_offset - structure_offset < structure_limit)
			structure_limit = reservations_offset - structure_offset;
		structure_declared = structure_limit;
	}

	state = (struct dt_state){
		.blob           = bytes,
		.blob_size      = total,
		.structure      = bytes + structure_offset,
		.structure_size = structure_limit,
		.strings        = bytes + strings_offset,
		.strings_size   = strings_size,
		.version        = version,
	};
	if (!dt_structure_validate(&state, structure_limit, &structure_size) ||
	    !dt_reservations_validate(bytes, total, reservations_offset, &reservations_size))
		return false;
	if (version >= 17u) {
		for (size_t index = structure_size; index < structure_declared; index++)
			if (state.structure[index] != 0u) return false;
	}
	else structure_declared = structure_size;
	if (dt_overlap(0u, header_size, structure_offset, structure_declared) ||
	    dt_overlap(0u, header_size, strings_offset, strings_size) ||
	    dt_overlap(0u, header_size, reservations_offset, reservations_size) ||
	    dt_overlap(structure_offset, structure_declared, strings_offset, strings_size) ||
	    dt_overlap(structure_offset, structure_declared, reservations_offset, reservations_size) ||
	    dt_overlap(strings_offset, strings_size, reservations_offset, reservations_size))
		return false;

	state.structure_size = structure_size;
	dt                   = state;
	__atomic_store_n(&dt_initialized, true, __ATOMIC_RELEASE);
	return true;
}

bool dt_node_valid(struct dt_node node) {
	size_t cursor = 0u;

	if (!__atomic_load_n(&dt_initialized, __ATOMIC_ACQUIRE) || node.id >= dt.structure_size || (node.id & 3u) != 0u)
		return false;
	while (cursor <= node.id) {
		struct dt_token token;

		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return false;
		if (cursor == node.id) return token.type == DT_BEGIN_NODE;
		if (token.next <= cursor || token.next > node.id) return false;
		cursor = token.next;
	}
	return false;
}

struct dt_node dt_root(void) {
	struct dt_token token;
	size_t          cursor = 0u;

	if (!__atomic_load_n(&dt_initialized, __ATOMIC_ACQUIRE)) return DT_NODE_INVALID;
	while (cursor < dt.structure_size) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) return (struct dt_node){.id = cursor};
		if (token.type != DT_NOP) return DT_NODE_INVALID;
		cursor = token.next;
	}
	return DT_NODE_INVALID;
}

struct dt_node dt_node_child(struct dt_node node) {
	struct dt_token token;
	size_t          cursor;

	if (!dt_node_valid(node) || !dt_token_read(&dt, node.id, dt.structure_size, &token)) return DT_NODE_INVALID;
	cursor = token.next;
	while (cursor < dt.structure_size) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) return (struct dt_node){.id = cursor};
		if (token.type == DT_END_NODE || token.type == DT_END) return DT_NODE_INVALID;
		cursor = token.next;
	}
	return DT_NODE_INVALID;
}

struct dt_node dt_node_next(struct dt_node node) {
	struct dt_token token;
	size_t          cursor;
	size_t          depth = 1u;

	if (!dt_node_valid(node) || !dt_token_read(&dt, node.id, dt.structure_size, &token)) return DT_NODE_INVALID;
	cursor = token.next;
	while (cursor < dt.structure_size && depth != 0u) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) depth++;
		else if (token.type == DT_END_NODE) depth--;
		else if (token.type == DT_END) return DT_NODE_INVALID;
		cursor = token.next;
	}
	while (cursor < dt.structure_size) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) return (struct dt_node){.id = cursor};
		if (token.type == DT_END_NODE || token.type == DT_END) return DT_NODE_INVALID;
		cursor = token.next;
	}
	return DT_NODE_INVALID;
}

struct dt_node dt_node_parent(struct dt_node node) {
	struct dt_node  parent = DT_NODE_INVALID;
	struct dt_node  root;
	struct dt_token token;
	size_t          target_depth = 0u;
	size_t          cursor       = 0u;
	size_t          depth        = 0u;

	if (!dt_node_valid(node)) return DT_NODE_INVALID;
	root = dt_root();
	if (!dt_node_valid(root) || node.id == root.id) return DT_NODE_INVALID;
	while (cursor < node.id) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) depth++;
		else if (token.type == DT_END_NODE) {
			if (depth == 0u) return DT_NODE_INVALID;
			depth--;
		}
		cursor = token.next;
	}
	if (cursor != node.id || depth == 0u) return DT_NODE_INVALID;
	target_depth = depth;
	cursor       = 0u;
	depth        = 0u;
	while (cursor < node.id) {
		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type == DT_BEGIN_NODE) {
			if (depth == target_depth - 1u) parent = (struct dt_node){.id = cursor};
			depth++;
		}
		else if (token.type == DT_END_NODE) {
			if (depth == 0u) return DT_NODE_INVALID;
			depth--;
			if (depth == target_depth - 1u) parent = DT_NODE_INVALID;
		}
		cursor = token.next;
	}
	return parent;
}

const char* dt_node_name(struct dt_node node) {
	struct dt_token token;

	if (!dt_node_valid(node) || !dt_token_read(&dt, node.id, dt.structure_size, &token)) return NULL;
	return (const char*)dt.structure + token.data;
}

static bool dt_node_property_find(struct dt_node node, size_t index, const char* name, const char** out_name,
                                  struct dt_property* out) {
	struct dt_token token;
	size_t          cursor;
	size_t          depth          = 0u;
	size_t          property_index = 0u;

	if (!dt_node_valid(node) || !dt_token_read(&dt, node.id, dt.structure_size, &token)) return false;
	cursor = token.next;
	while (cursor < dt.structure_size) {
		const char* property_name;

		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return false;
		if (token.type == DT_BEGIN_NODE) depth++;
		else if (token.type == DT_END_NODE) {
			if (depth == 0u) return false;
			depth--;
		}
		else if (token.type == DT_END) return false;
		else if (token.type == DT_PROPERTY && depth == 0u) {
			property_name = (const char*)dt.strings + token.name_offset;
			if ((name != NULL && strcmp(property_name, name) != 0) || (name == NULL && property_index++ != index)) {
				cursor = token.next;
				continue;
			}
			if (out_name != NULL) *out_name = property_name;
			*out = (struct dt_property){.data = dt.structure + token.data, .size = token.size};
			return true;
		}
		cursor = token.next;
	}
	return false;
}

bool dt_node_property_at(struct dt_node node, size_t index, const char** out_name, struct dt_property* out) {
	if (out_name == NULL || out == NULL) return false;
	return dt_node_property_find(node, index, NULL, out_name, out);
}

bool dt_node_property(struct dt_node node, const char* name, struct dt_property* out) {
	if (name == NULL || out == NULL) return false;
	return dt_node_property_find(node, 0u, name, NULL, out);
}

bool dt_property_read_cells(const struct dt_property* property, size_t first_cell, size_t cell_count, uint64_t* out) {
	const uint8_t* bytes;
	uint64_t       value = 0u;
	size_t         offset;

	if (property == NULL || property->data == NULL || out == NULL || cell_count == 0u || cell_count > 2u ||
	    first_cell > SIZE_MAX / 4u)
		return false;
	offset = first_cell * 4u;
	if (offset > property->size || cell_count > (property->size - offset) / 4u) return false;
	bytes = (const uint8_t*)property->data + offset;
	for (size_t index = 0u; index < cell_count; index++) value = (value << 32u) | dt_read_u32(bytes + index * 4u);
	*out = value;
	return true;
}

bool dt_property_string_list_contains(const struct dt_property* property, const char* value) {
	const uint8_t* bytes;
	size_t         remaining;
	bool           found = false;

	if (property == NULL || property->data == NULL || value == NULL) return false;
	bytes     = property->data;
	remaining = property->size;
	while (remaining != 0u) {
		const uint8_t* end = memchr(bytes, '\0', remaining);
		size_t         size;

		if (end == NULL) return false;
		size = (size_t)(end - bytes) + 1u;
		if (strcmp((const char*)bytes, value) == 0) found = true;
		bytes += size;
		remaining -= size;
	}
	return found;
}

bool dt_node_enabled(struct dt_node node) {
	struct dt_property status;

	if (!dt_node_valid(node)) return false;
	if (!dt_node_property(node, "status", &status)) return true;
	return (status.size == 3u && memcmp(status.data, "ok\0", 3u) == 0) ||
	       (status.size == 5u && memcmp(status.data, "okay\0", 5u) == 0);
}

bool dt_node_compatible(struct dt_node node, const char* compatible) {
	struct dt_property property;

	return compatible != NULL && dt_node_property(node, "compatible", &property) &&
	       dt_property_string_list_contains(&property, compatible);
}

struct dt_node dt_node_by_phandle(uint32_t phandle) {
	struct dt_node found  = DT_NODE_INVALID;
	size_t         cursor = 0u;

	if (phandle == 0u || !__atomic_load_n(&dt_initialized, __ATOMIC_ACQUIRE)) return DT_NODE_INVALID;
	while (cursor < dt.structure_size) {
		struct dt_token    token;
		struct dt_node     node;
		struct dt_property standard;
		struct dt_property legacy;
		uint64_t           standard_value = 0u;
		uint64_t           legacy_value   = 0u;
		bool               has_standard;
		bool               has_legacy;

		if (!dt_token_read(&dt, cursor, dt.structure_size, &token)) return DT_NODE_INVALID;
		if (token.type != DT_BEGIN_NODE) {
			cursor = token.next;
			continue;
		}
		node         = (struct dt_node){.id = cursor};
		has_standard = dt_node_property(node, "phandle", &standard);
		has_legacy   = dt_node_property(node, "linux,phandle", &legacy);
		if ((has_standard && !dt_property_read_cells(&standard, 0u, 1u, &standard_value)) ||
		    (has_legacy && !dt_property_read_cells(&legacy, 0u, 1u, &legacy_value)) ||
		    (has_standard && standard.size != 4u) || (has_legacy && legacy.size != 4u) ||
		    (has_standard && has_legacy && standard_value != legacy_value))
			return DT_NODE_INVALID;
		if ((has_standard && standard_value == phandle) || (has_legacy && legacy_value == phandle)) {
			if (dt_node_valid(found)) return DT_NODE_INVALID;
			found = node;
		}
		cursor = token.next;
	}
	return found;
}
