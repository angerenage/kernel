#include "parser.h"

#include <base/device_tree.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <system/capability.h>
#include <system/device_tree.h>

#include "property.h"
#include "tree.h"

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

static syscall_status_t append_registers(const struct device_builder* builder, const struct dt_parser_ranges* ranges) {
	uint64_t         element_count;
	uint64_t         value_size;
	syscall_status_t status;

	if (ranges->count == 0u || ranges->count > UINT64_MAX / 2u) return SYSCALL_STATUS_BAD_ARGUMENT;
	element_count = (uint64_t)ranges->count * 2u;
	if (element_count > UINT64_MAX / sizeof(uint64_t)) return SYSCALL_STATUS_BAD_ARGUMENT;
	value_size = element_count * sizeof(uint64_t);
	status     = device_builder_begin_property(
		builder, "registers", sizeof("registers") - 1u, DEVICE_PROPERTY_UNSIGNED_ARRAY, element_count, value_size);
	for (size_t index = 0u; status == SYSCALL_STATUS_OK && index < ranges->count; index++) {
		uint8_t pair[2u * sizeof(uint64_t)];
		dt_parser_write_u64_le(pair, ranges->values[index].base);
		dt_parser_write_u64_le(pair + sizeof(uint64_t), ranges->values[index].length);
		status = device_builder_append_property(builder, index * sizeof(pair), pair, sizeof(pair));
	}
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static syscall_status_t submit_node(cap_id_t provider_cap, cap_id_t root_cap, device_tree_node_id_t node,
                                    bool* out_committed) {
	struct device_tree_node_info_response info;
	struct dt_parser_string_list          compatibles = {0};
	struct dt_parser_ranges               ranges      = {0};
	struct dt_parser_name_set             names       = {0};
	struct device_builder                 builder     = {
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
	if (status == SYSCALL_STATUS_OK) registers = true;
	else if (status != SYSCALL_STATUS_UNAVAILABLE) goto cleanup;
	status = device_builder_begin_root(root_cap, &builder);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	status = add_compatibles(&builder, &compatibles);
	if (status == SYSCALL_STATUS_OK) status = set_node_name(provider_cap, node, &info, &builder);
	if (status == SYSCALL_STATUS_OK && registers) {
		status = dt_parser_name_set_add(&names, "registers", sizeof("registers") - 1u);
		if (status == SYSCALL_STATUS_OK) status = append_registers(&builder, &ranges);
	}
	for (uint64_t index = 0u; status == SYSCALL_STATUS_OK && index < info.property_count; index++)
		status = dt_parser_property_append(provider_cap, node, index, registers, &builder, &names);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status == SYSCALL_STATUS_OK) *out_committed = true;
	else {
		(void)device_builder_abort(&builder);
		if (status == SYSCALL_STATUS_BAD_ARGUMENT || status == SYSCALL_STATUS_UNAVAILABLE) status = SYSCALL_STATUS_OK;
	}

cleanup:
	dt_parser_name_set_deinit(&names);
	dt_parser_ranges_deinit(&ranges);
	dt_parser_string_list_deinit(&compatibles);
	return status;
}

syscall_status_t dt_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, size_t* device_count) {
	device_tree_node_id_t node;
	syscall_status_t      status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
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
			status = submit_node(provider_cap, root_cap, node, &committed);
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
