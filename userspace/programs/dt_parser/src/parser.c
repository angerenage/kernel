#include "parser.h"

#include <base/device_tree.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/device_tree.h>

#include "property.h"
#include "tree.h"

#define DT_DEFAULT_REGISTER_RESOURCE "registers"

struct dt_parser_resource_names {
	char (*values)[DEVICE_IDENTIFIER_MAX + 1u];
	uint8_t* sizes;
	size_t   count;
};

static syscall_status_t add_compatibles(const struct device_builder*        builder,
                                        const struct dt_parser_string_list* compatibles) {
	size_t offset = 0u;

	for (size_t index = 0u; index < compatibles->count; index++) {
		uint32_t length;

		if (compatibles->value_size - offset < sizeof(length)) return SYSCALL_STATUS_FAILED;
		length = dt_parser_read_u32_le(compatibles->value + offset);
		offset += sizeof(length);
		if (length == 0u || length > compatibles->value_size - offset) return SYSCALL_STATUS_FAILED;
		syscall_status_t status =
			device_builder_add_compatible(builder, (const char*)compatibles->value + offset, length);
		if (status != SYSCALL_STATUS_OK) return status;
		offset += length;
	}
	return offset == compatibles->value_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

static syscall_status_t set_node_name(cap_id_t provider_cap, device_tree_node_id_t node,
                                      const struct device_tree_node_info_response* info,
                                      const struct device_builder*                 builder) {
	char*            name;
	syscall_status_t status;

	if (info->name_size == 0u || info->name_size > DEVICE_NAME_MAX || info->name_size > SIZE_MAX)
		return SYSCALL_STATUS_OK;
	name = malloc((size_t)info->name_size);
	if (name == NULL) return SYSCALL_STATUS_FAILED;
	status = device_tree_node_name_read(provider_cap, node, 0u, name, (size_t)info->name_size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(builder, name, (size_t)info->name_size);
	free(name);
	return status == SYSCALL_STATUS_BAD_ARGUMENT ? SYSCALL_STATUS_OK : status;
}

static void resource_names_deinit(struct dt_parser_resource_names* names) {
	if (names == NULL) return;
	free(names->sizes);
	free(names->values);
	*names = (struct dt_parser_resource_names){0};
}

static void resource_name_default(char output[DEVICE_IDENTIFIER_MAX + 1u], uint8_t* out_size, size_t index,
                                  size_t count) {
	static const char prefix[] = DT_DEFAULT_REGISTER_RESOURCE "_";
	char              digits[3u * sizeof(size_t)];
	size_t            digit_count = 0u;
	size_t            written;

	if (count == 1u) {
		memcpy(output, DT_DEFAULT_REGISTER_RESOURCE, sizeof(DT_DEFAULT_REGISTER_RESOURCE) - 1u);
		*out_size = sizeof(DT_DEFAULT_REGISTER_RESOURCE) - 1u;
		return;
	}
	do {
		digits[digit_count++] = (char)('0' + index % 10u);
		index /= 10u;
	} while (index != 0u);
	memcpy(output, prefix, sizeof(prefix) - 1u);
	written = sizeof(prefix) - 1u;
	while (digit_count != 0u) output[written++] = digits[--digit_count];
	*out_size = (uint8_t)written;
}

static syscall_status_t resource_names_read(cap_id_t provider_cap, device_tree_node_id_t node, size_t count,
                                            struct dt_parser_resource_names* out_names) {
	struct dt_parser_property property;
	uint8_t*                  raw = NULL;
	size_t                    offset;
	syscall_status_t          status;

	if (out_names == NULL || count == 0u || count > DEVICE_MAX_RESOURCES) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_names        = (struct dt_parser_resource_names){0};
	out_names->values = calloc(count, sizeof(*out_names->values));
	out_names->sizes  = calloc(count, sizeof(*out_names->sizes));
	if (out_names->values == NULL || out_names->sizes == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	out_names->count = count;
	status           = dt_parser_property_find(provider_cap, node, "reg-names", &property);
	if (status == SYSCALL_STATUS_UNAVAILABLE) {
		for (size_t index = 0u; index < count; index++)
			resource_name_default(out_names->values[index], &out_names->sizes[index], index, count);
		return SYSCALL_STATUS_OK;
	}
	if (status != SYSCALL_STATUS_OK) goto fail;
	if (property.value_size == 0u || property.value_size > SIZE_MAX || property.value_size > DEVICE_MAX_STATE_SIZE) {
		status = SYSCALL_STATUS_UNAVAILABLE;
		goto fail;
	}
	raw = malloc((size_t)property.value_size);
	if (raw == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	status = dt_parser_property_read(provider_cap, node, &property, 0u, raw, (size_t)property.value_size);
	if (status != SYSCALL_STATUS_OK) goto fail;
	offset = 0u;
	for (size_t index = 0u; index < count; index++) {
		uint8_t* end;
		size_t   source_size;
		size_t   normalized_size;

		if (offset == property.value_size) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail;
		}
		end = memchr(raw + offset, 0, (size_t)property.value_size - offset);
		if (end == NULL) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail;
		}
		source_size = (size_t)(end - raw - offset);
		if (source_size == 0u || !dt_parser_utf8_valid(raw + offset, source_size) ||
		    !dt_parser_name_normalize(
				(const char*)raw + offset, source_size, out_names->values[index], &normalized_size)) {
			status = SYSCALL_STATUS_UNAVAILABLE;
			goto fail;
		}
		out_names->sizes[index] = (uint8_t)normalized_size;
		for (size_t previous = 0u; previous < index; previous++)
			if (out_names->sizes[previous] == normalized_size &&
			    memcmp(out_names->values[previous], out_names->values[index], normalized_size) == 0) {
				status = SYSCALL_STATUS_UNAVAILABLE;
				goto fail;
			}
		offset += source_size + 1u;
	}
	if (offset != property.value_size) {
		status = SYSCALL_STATUS_UNAVAILABLE;
		goto fail;
	}
	free(raw);
	return SYSCALL_STATUS_OK;

fail:
	free(raw);
	resource_names_deinit(out_names);
	return status;
}

static syscall_status_t add_mmio_resources(const struct device_builder* builder, cap_id_t memory_allocator_cap,
                                           const struct dt_parser_ranges*         ranges,
                                           const struct dt_parser_resource_names* resource_names) {
	if (ranges == NULL || resource_names == NULL || ranges->count != resource_names->count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	for (size_t index = 0u; index < ranges->count; index++) {
		if (ranges->values[index].base > UINTPTR_MAX || ranges->values[index].length > SIZE_MAX)
			return SYSCALL_STATUS_UNAVAILABLE;
		syscall_status_t status = device_builder_add_mmio_resource(builder,
		                                                           memory_allocator_cap,
		                                                           resource_names->values[index],
		                                                           resource_names->sizes[index],
		                                                           (uintptr_t)ranges->values[index].base,
		                                                           (size_t)ranges->values[index].length);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t submit_node(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                    device_tree_node_id_t node, bool* out_committed) {
	struct device_tree_node_info_response info;
	struct dt_parser_string_list          compatibles    = {0};
	struct dt_parser_ranges               ranges         = {0};
	struct dt_parser_resource_names       resource_names = {0};
	struct dt_parser_name_set             names          = {0};
	struct device_builder                 builder        = {
		.capability  = CAP_ID_INVALID,
		.manager_pid = PROCESS_PID_INVALID,
	};
	bool             registers = false;
	syscall_status_t status;

	if (out_committed == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_committed = false;
	status         = device_tree_node_info(provider_cap, node, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	status = dt_parser_compatibles_read(provider_cap, node, &compatibles);
	if (status == SYSCALL_STATUS_UNAVAILABLE) return SYSCALL_STATUS_OK;
	if (status != SYSCALL_STATUS_OK) return status;
	status = dt_parser_ranges_read(provider_cap, node, &ranges);
	if (status == SYSCALL_STATUS_OK) {
		status = resource_names_read(provider_cap, node, ranges.count, &resource_names);
		if (status != SYSCALL_STATUS_OK) goto cleanup;
		registers = true;
	}
	else if (status != SYSCALL_STATUS_UNAVAILABLE) goto cleanup;
	status = device_builder_begin_root(root_cap, &builder);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	status = add_compatibles(&builder, &compatibles);
	if (status == SYSCALL_STATUS_OK) status = set_node_name(provider_cap, node, &info, &builder);
	if (status == SYSCALL_STATUS_OK && registers)
		status = add_mmio_resources(&builder, memory_allocator_cap, &ranges, &resource_names);
	for (uint64_t index = 0u; status == SYSCALL_STATUS_OK && index < info.property_count; index++)
		status = dt_parser_property_append(provider_cap, node, index, registers, &builder, &names);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status == SYSCALL_STATUS_OK) *out_committed = true;
	else {
		(void)device_builder_abort(&builder);
		if (status == SYSCALL_STATUS_BAD_ARGUMENT || status == SYSCALL_STATUS_UNAVAILABLE) status = SYSCALL_STATUS_OK;
	}

cleanup:
	if (status != SYSCALL_STATUS_OK && builder.capability != CAP_ID_INVALID) (void)device_builder_abort(&builder);
	if (status == SYSCALL_STATUS_BAD_ARGUMENT || status == SYSCALL_STATUS_UNAVAILABLE) status = SYSCALL_STATUS_OK;
	dt_parser_name_set_deinit(&names);
	resource_names_deinit(&resource_names);
	dt_parser_ranges_deinit(&ranges);
	dt_parser_string_list_deinit(&compatibles);
	return status;
}

syscall_status_t dt_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                 size_t* device_count) {
	device_tree_node_id_t node;
	syscall_status_t      status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID || memory_allocator_cap == CAP_ID_INVALID)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (device_count != NULL) *device_count = 0u;
	status = device_tree_root(provider_cap, &node);
	if (status != SYSCALL_STATUS_OK) return status;
	while (node != DEVICE_TREE_NODE_INVALID) {
		device_tree_node_id_t next;
		bool                  enabled;
		bool                  committed;

		status = dt_parser_node_enabled(provider_cap, node, &enabled);
		if (status != SYSCALL_STATUS_OK) return status;
		if (enabled) {
			status = submit_node(provider_cap, root_cap, memory_allocator_cap, node, &committed);
			if (status != SYSCALL_STATUS_OK) return status;
			if (committed && device_count != NULL) {
				if (*device_count == SIZE_MAX) return SYSCALL_STATUS_FAILED;
				(*device_count)++;
			}
		}
		status = dt_parser_walk_next(provider_cap, node, &next);
		if (status != SYSCALL_STATUS_OK) return status;
		node = next;
	}
	return SYSCALL_STATUS_OK;
}
