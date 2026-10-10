#include "property.h"

#include <protocol/device.h>
#include <stdlib.h>
#include <string.h>
#include <system/device_tree.h>

#include "tree.h"

static bool source_name_is(const char* name, size_t name_size, const char* expected) {
	size_t expected_size = strlen(expected);
	return name_size == expected_size && memcmp(name, expected, name_size) == 0;
}

bool dt_parser_name_normalize(const char* source, size_t source_size, char output[DEVICE_IDENTIFIER_MAX],
                              size_t* out_size) {
	size_t written   = 0u;
	bool   separator = false;

	if (source == NULL || source_size == 0u || output == NULL || out_size == NULL) return false;
	for (size_t index = 0u; index < source_size; index++) {
		unsigned char byte = (unsigned char)source[index];
		char          normalized;

		if (byte >= 'A' && byte <= 'Z') normalized = (char)(byte - 'A' + 'a');
		else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) normalized = (char)byte;
		else {
			separator = written != 0u;
			continue;
		}
		if (written == 0u && normalized >= '0' && normalized <= '9') {
			output[written++] = 'd';
			output[written++] = 't';
			output[written++] = '_';
		}
		if (separator && written != 0u && output[written - 1u] != '_') {
			if (written == DEVICE_IDENTIFIER_MAX) return false;
			output[written++] = '_';
		}
		if (written == DEVICE_IDENTIFIER_MAX) return false;
		output[written++] = normalized;
		separator         = false;
	}
	while (written != 0u && output[written - 1u] == '_') written--;
	if (written == 0u || output[0] < 'a' || output[0] > 'z') return false;
	*out_size = written;
	return true;
}

syscall_status_t dt_parser_name_set_add(struct dt_parser_name_set* names, const char* name, size_t name_size) {
	struct dt_parser_name* item;

	if (names == NULL || name == NULL || name_size == 0u || name_size > DEVICE_IDENTIFIER_MAX)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	for (item = names->first; item != NULL; item = item->next)
		if (item->size == name_size && memcmp(item->value, name, name_size) == 0) return SYSCALL_STATUS_BAD_ARGUMENT;
	item = malloc(sizeof(*item) + name_size);
	if (item == NULL) return SYSCALL_STATUS_FAILED;
	item->size = name_size;
	memcpy(item->value, name, name_size);
	item->next   = names->first;
	names->first = item;
	return SYSCALL_STATUS_OK;
}

void dt_parser_name_set_deinit(struct dt_parser_name_set* names) {
	if (names == NULL) return;
	while (names->first != NULL) {
		struct dt_parser_name* item = names->first;
		names->first                = item->next;
		free(item);
	}
}

static syscall_status_t append_chunks(const struct device_builder* builder, const uint8_t* value, size_t value_size) {
	size_t offset = 0u;

	while (offset < value_size) {
		size_t chunk = value_size - offset;
		if (chunk > CAP_MAX_REQUEST_SIZE - sizeof(struct device_builder_append_property_request))
			chunk = CAP_MAX_REQUEST_SIZE - sizeof(struct device_builder_append_property_request);
		syscall_status_t status = device_builder_append_property(builder, offset, value + offset, chunk);
		if (status != SYSCALL_STATUS_OK) return status;
		offset += chunk;
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t append_boolean(const struct device_builder* builder, const char* name, size_t name_size) {
	const uint8_t    value = 1u;
	syscall_status_t status =
		device_builder_begin_property(builder, name, name_size, DEVICE_PROPERTY_BOOLEAN, 1u, sizeof(value));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, &value, sizeof(value));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static bool string_list_shape(const uint8_t* value, size_t value_size, size_t* out_count, size_t* out_encoded_size) {
	size_t count  = 0u;
	size_t offset = 0u;

	if (value == NULL || value_size == 0u || value[value_size - 1u] != 0u) return false;
	while (offset < value_size) {
		const uint8_t* end = memchr(value + offset, 0, value_size - offset);
		size_t         length;

		if (end == NULL) return false;
		length = (size_t)(end - value - offset);
		if (length == 0u || !dt_parser_utf8_valid(value + offset, length)) return false;
		if (count == SIZE_MAX) return false;
		count++;
		offset += length + 1u;
	}
	if (count > (SIZE_MAX - value_size) / 3u) return false;
	*out_count        = count;
	*out_encoded_size = value_size + count * 3u;
	return true;
}

static syscall_status_t append_string_value(const struct device_builder* builder, const char* name, size_t name_size,
                                            const uint8_t* value, size_t value_size, size_t count,
                                            size_t encoded_size) {
	uint8_t*         encoded;
	size_t           source_offset  = 0u;
	size_t           encoded_offset = 0u;
	syscall_status_t status;

	if (count == 1u) {
		status =
			device_builder_begin_property(builder, name, name_size, DEVICE_PROPERTY_UTF8_STRING, 1u, value_size - 1u);
		if (status == SYSCALL_STATUS_OK) status = append_chunks(builder, value, value_size - 1u);
		if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
		return status;
	}
	encoded = malloc(encoded_size);
	if (encoded == NULL) return SYSCALL_STATUS_FAILED;
	while (source_offset < value_size) {
		size_t length = strlen((const char*)value + source_offset);
		dt_parser_write_u32_le(encoded + encoded_offset, (uint32_t)length);
		encoded_offset += sizeof(uint32_t);
		memcpy(encoded + encoded_offset, value + source_offset, length);
		encoded_offset += length;
		source_offset += length + 1u;
	}
	status =
		device_builder_begin_property(builder, name, name_size, DEVICE_PROPERTY_UTF8_STRING_ARRAY, count, encoded_size);
	if (status == SYSCALL_STATUS_OK) status = append_chunks(builder, encoded, encoded_size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	free(encoded);
	return status;
}

static syscall_status_t append_bytes(const struct device_builder* builder, const char* name, size_t name_size,
                                     const uint8_t* value, size_t value_size) {
	syscall_status_t status =
		device_builder_begin_property(builder, name, name_size, DEVICE_PROPERTY_BYTES, value_size, value_size);
	if (status == SYSCALL_STATUS_OK) status = append_chunks(builder, value, value_size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

syscall_status_t dt_parser_property_append(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           bool registers_translated, bool dma_translated, bool interrupts_translated,
                                           const struct device_builder* builder, struct dt_parser_name_set* names) {
	struct device_tree_property_info_response info;
	struct dt_parser_property                 property;
	char*                                     source_name;
	uint8_t*                                  value = NULL;
	char                                      normalized[DEVICE_IDENTIFIER_MAX];
	size_t                                    normalized_size;
	size_t                                    string_count;
	size_t                                    encoded_size;
	syscall_status_t                          status;

	status = device_tree_property_info(provider_cap, node, property_index, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.name_size == 0u || info.name_size > SIZE_MAX || info.name_size > DEVICE_MAX_STATE_SIZE)
		return SYSCALL_STATUS_OK;
	source_name = malloc((size_t)info.name_size);
	if (source_name == NULL) return SYSCALL_STATUS_FAILED;
	status =
		device_tree_property_name_read(provider_cap, node, property_index, 0u, source_name, (size_t)info.name_size);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (source_name_is(source_name, (size_t)info.name_size, "compatible") ||
	    source_name_is(source_name, (size_t)info.name_size, "status") ||
	    (registers_translated && (source_name_is(source_name, (size_t)info.name_size, "reg") ||
	                              source_name_is(source_name, (size_t)info.name_size, "reg-names"))) ||
	    (dma_translated && source_name_is(source_name, (size_t)info.name_size, "iommus")) ||
	    (interrupts_translated && (source_name_is(source_name, (size_t)info.name_size, "interrupts") ||
	                               source_name_is(source_name, (size_t)info.name_size, "interrupts-extended") ||
	                               source_name_is(source_name, (size_t)info.name_size, "interrupt-parent") ||
	                               source_name_is(source_name, (size_t)info.name_size, "interrupt-names")))) {
		status = SYSCALL_STATUS_OK;
		goto cleanup;
	}
	if (!dt_parser_name_normalize(source_name, (size_t)info.name_size, normalized, &normalized_size)) {
		status = SYSCALL_STATUS_OK;
		goto cleanup;
	}
	status = dt_parser_name_set_add(names, normalized, normalized_size);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (info.value_size > SIZE_MAX || info.value_size > DEVICE_MAX_STATE_SIZE) {
		status = SYSCALL_STATUS_UNAVAILABLE;
		goto cleanup;
	}
	property = (struct dt_parser_property){.index = property_index, .value_size = info.value_size};
	if (info.value_size == 0u) {
		status = append_boolean(builder, normalized, normalized_size);
		goto cleanup;
	}
	value = malloc((size_t)info.value_size);
	if (value == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto cleanup;
	}
	status = dt_parser_property_read(provider_cap, node, &property, 0u, value, (size_t)info.value_size);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (string_list_shape(value, (size_t)info.value_size, &string_count, &encoded_size))
		status = append_string_value(
			builder, normalized, normalized_size, value, (size_t)info.value_size, string_count, encoded_size);
	else status = append_bytes(builder, normalized, normalized_size, value, (size_t)info.value_size);

cleanup:
	free(value);
	free(source_name);
	return status;
}
