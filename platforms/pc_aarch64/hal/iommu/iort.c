#include "iort.h"

#include <firmware/acpi.h>
#include <stddef.h>
#include <string.h>

#define IORT_NODE_HEADER_SIZE 16u
#define IORT_ID_MAPPING_SIZE 20u
#define IORT_NODE_PCI_ROOT_COMPLEX 2u
#define IORT_NODE_SMMUV3 4u
#define IORT_ID_SINGLE_MAPPING 1u

static bool iort_read_u16(const uint8_t* bytes, size_t size, size_t offset, uint16_t* out) {
	if (bytes == NULL || out == NULL || offset > size || sizeof(*out) > size - offset) return false;
	memcpy(out, bytes + offset, sizeof(*out));
	return true;
}

static bool iort_read_u32(const uint8_t* bytes, size_t size, size_t offset, uint32_t* out) {
	if (bytes == NULL || out == NULL || offset > size || sizeof(*out) > size - offset) return false;
	memcpy(out, bytes + offset, sizeof(*out));
	return true;
}

static bool iort_read_u64(const uint8_t* bytes, size_t size, size_t offset, uint64_t* out) {
	if (bytes == NULL || out == NULL || offset > size || sizeof(*out) > size - offset) return false;
	memcpy(out, bytes + offset, sizeof(*out));
	return true;
}

static bool iort_node(const uint8_t* table, size_t table_size, uint32_t offset, const uint8_t** out_node,
                      uint16_t* out_length) {
	uint16_t length;
	if (out_node == NULL || out_length == NULL || offset > table_size || IORT_NODE_HEADER_SIZE > table_size - offset ||
	    !iort_read_u16(table, table_size, (size_t)offset + 1u, &length) || length < IORT_NODE_HEADER_SIZE ||
	    length > table_size - offset)
		return false;
	*out_node   = table + offset;
	*out_length = length;
	return true;
}

static bool iort_translate_id(const uint8_t* node, uint16_t length, uint32_t input_id, uint32_t* out_id,
                              uint32_t* out_reference) {
	uint32_t mapping_count;
	uint32_t mapping_offset;
	bool     matched = false;

	if (node == NULL || out_id == NULL || out_reference == NULL || !iort_read_u32(node, length, 8u, &mapping_count) ||
	    !iort_read_u32(node, length, 12u, &mapping_offset) || mapping_count == 0u ||
	    mapping_offset < IORT_NODE_HEADER_SIZE || mapping_offset > length ||
	    mapping_count > (length - mapping_offset) / IORT_ID_MAPPING_SIZE)
		return false;
	for (uint32_t index = 0u; index < mapping_count; index++) {
		size_t   offset = (size_t)mapping_offset + (size_t)index * IORT_ID_MAPPING_SIZE;
		uint32_t input_base;
		uint32_t id_count;
		uint32_t output_base;
		uint32_t output_reference;
		uint32_t flags;
		uint32_t translated;

		if (!iort_read_u32(node, length, offset, &input_base) || !iort_read_u32(node, length, offset + 4u, &id_count) ||
		    !iort_read_u32(node, length, offset + 8u, &output_base) ||
		    !iort_read_u32(node, length, offset + 12u, &output_reference) ||
		    !iort_read_u32(node, length, offset + 16u, &flags) || output_reference == 0u)
			return false;
		if ((flags & IORT_ID_SINGLE_MAPPING) != 0u) translated = output_base;
		else {
			if (input_id < input_base || input_id - input_base > id_count) continue;
			if (output_base > UINT32_MAX - (input_id - input_base)) return false;
			translated = output_base + (input_id - input_base);
		}
		if (matched) return false;
		*out_id        = translated;
		*out_reference = output_reference;
		matched        = true;
	}
	return matched;
}

bool aarch64_iort_pci_route(uint16_t segment, uint16_t requester_id, uint64_t* out_controller_address,
                            uint32_t* out_source_id) {
	const struct acpi_sdt_header* header = acpi_table_next("IORT", NULL);
	const uint8_t*                table  = (const uint8_t*)header;
	uint32_t                      node_count;
	uint32_t                      node_offset;
	const uint8_t*                root = NULL;
	uint16_t                      root_length;

	if (header == NULL || out_controller_address == NULL || out_source_id == NULL || header->length < 48u ||
	    !iort_read_u32(table, header->length, 36u, &node_count) ||
	    !iort_read_u32(table, header->length, 40u, &node_offset) || node_offset < 48u)
		return false;
	uint32_t cursor = node_offset;
	for (uint32_t index = 0u; index < node_count; index++) {
		const uint8_t* node;
		uint16_t       length;
		uint32_t       pci_segment;
		if (!iort_node(table, header->length, cursor, &node, &length)) return false;
		if (node[0] == IORT_NODE_PCI_ROOT_COMPLEX && length >= 32u && iort_read_u32(node, length, 28u, &pci_segment) &&
		    pci_segment == segment) {
			if (root != NULL) return false;
			root        = node;
			root_length = length;
		}
		if (length > UINT32_MAX - cursor) return false;
		cursor += length;
	}
	if (root == NULL) return false;
	uint32_t source_id = requester_id;
	for (uint32_t depth = 0u; depth < node_count; depth++) {
		uint32_t reference;
		if (!iort_translate_id(root, root_length, source_id, &source_id, &reference) ||
		    !iort_node(table, header->length, reference, &root, &root_length))
			return false;
		if (root[0] == IORT_NODE_SMMUV3) {
			uint64_t address;
			if (root_length < 24u || !iort_read_u64(root, root_length, 16u, &address) || address == 0u) return false;
			*out_controller_address = address;
			*out_source_id          = source_id;
			return true;
		}
	}
	return false;
}
