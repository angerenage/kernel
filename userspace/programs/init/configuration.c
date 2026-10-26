#include "configuration.h"

#include <base/cap.h>
#include <protocol/filesystem.h>
#include <runtime/filesystem.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <yaml.h>

#define INIT_CONFIGURATION_MAXIMUM_SIZE (1024u * 1024u)

static bool scalar_equal(const yaml_node_t* node, const char* expected) {
	size_t expected_size = strlen(expected);
	return node != NULL && node->type == YAML_SCALAR_NODE && node->data.scalar.length == expected_size &&
	       memcmp(node->data.scalar.value, expected, expected_size) == 0;
}

static enum init_configuration_status parse_layout_path(struct init_configuration* configuration,
                                                        const yaml_node_t*         node) {
	if (node == NULL || node->type != YAML_SCALAR_NODE || node->data.scalar.length == 0u ||
	    node->data.scalar.length >= sizeof(configuration->text_input_layout) ||
	    memchr(node->data.scalar.value, '\0', node->data.scalar.length) != NULL ||
	    !vfs_absolute_path_valid(node->data.scalar.value, node->data.scalar.length))
		return INIT_CONFIGURATION_INVALID;
	memcpy(configuration->text_input_layout, node->data.scalar.value, node->data.scalar.length);
	configuration->text_input_layout[node->data.scalar.length] = '\0';
	return INIT_CONFIGURATION_OK;
}

static enum init_configuration_status parse_text_input(struct init_configuration* configuration,
                                                       yaml_document_t* document, const yaml_node_t* node) {
	enum init_configuration_status status;
	bool                           have_layout = false;

	if (node == NULL || node->type != YAML_MAPPING_NODE) return INIT_CONFIGURATION_INVALID;
	for (yaml_node_pair_t* pair = node->data.mapping.pairs.start; pair != node->data.mapping.pairs.top; pair++) {
		yaml_node_t* key   = yaml_document_get_node(document, pair->key);
		yaml_node_t* value = yaml_document_get_node(document, pair->value);
		if (!scalar_equal(key, "keyboard_layout") || have_layout) return INIT_CONFIGURATION_INVALID;
		have_layout = true;
		status      = parse_layout_path(configuration, value);
		if (status != INIT_CONFIGURATION_OK) return status;
	}
	return have_layout ? INIT_CONFIGURATION_OK : INIT_CONFIGURATION_INVALID;
}

static bool service_character(char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
	       value == '_' || value == '-' || value == '.';
}

static enum init_configuration_status parse_service(char               output[INIT_CONFIGURATION_SERVICE_CAPACITY],
                                                    const yaml_node_t* node, bool normalize_uuid) {
	if (node == NULL || node->type != YAML_SCALAR_NODE || node->data.scalar.length == 0u ||
	    node->data.scalar.length >= INIT_CONFIGURATION_SERVICE_CAPACITY ||
	    memchr(node->data.scalar.value, '\0', node->data.scalar.length) != NULL)
		return INIT_CONFIGURATION_INVALID;
	for (size_t index = 0u; index < node->data.scalar.length; index++) {
		char value = (char)node->data.scalar.value[index];
		if (!service_character(value)) return INIT_CONFIGURATION_INVALID;
		if (normalize_uuid && value >= 'A' && value <= 'Z') value += 'a' - 'A';
		output[index] = value;
	}
	output[node->data.scalar.length] = '\0';
	return INIT_CONFIGURATION_OK;
}

static enum init_configuration_status parse_mount_point(char output[INIT_CONFIGURATION_MOUNT_PATH_CAPACITY],
                                                        const yaml_node_t* node) {
	if (node == NULL || node->type != YAML_SCALAR_NODE || node->data.scalar.length == 0u ||
	    node->data.scalar.length >= INIT_CONFIGURATION_MOUNT_PATH_CAPACITY ||
	    memchr(node->data.scalar.value, '\0', node->data.scalar.length) != NULL ||
	    !vfs_absolute_path_valid(node->data.scalar.value, node->data.scalar.length))
		return INIT_CONFIGURATION_INVALID;
	memcpy(output, node->data.scalar.value, node->data.scalar.length);
	output[node->data.scalar.length] = '\0';
	return INIT_CONFIGURATION_OK;
}

static enum init_configuration_status parse_mount(struct init_mount_configuration* mount, yaml_document_t* document,
                                                  const yaml_node_t* node) {
	bool have_identity = false;
	bool have_path     = false;
	bool have_readonly = false;
	if (node == NULL || node->type != YAML_MAPPING_NODE) return INIT_CONFIGURATION_INVALID;
	for (yaml_node_pair_t* pair = node->data.mapping.pairs.start; pair != node->data.mapping.pairs.top; pair++) {
		yaml_node_t* key   = yaml_document_get_node(document, pair->key);
		yaml_node_t* value = yaml_document_get_node(document, pair->value);
		if (scalar_equal(key, "uuid") || scalar_equal(key, "service")) {
			if (have_identity) return INIT_CONFIGURATION_INVALID;
			have_identity                         = true;
			enum init_configuration_status status = parse_service(mount->service, value, scalar_equal(key, "uuid"));
			if (status != INIT_CONFIGURATION_OK) return status;
		}
		else if (scalar_equal(key, "mount_point")) {
			if (have_path) return INIT_CONFIGURATION_INVALID;
			have_path                             = true;
			enum init_configuration_status status = parse_mount_point(mount->mount_point, value);
			if (status != INIT_CONFIGURATION_OK) return status;
		}
		else if (scalar_equal(key, "read_only")) {
			if (have_readonly || value == NULL || value->type != YAML_SCALAR_NODE) return INIT_CONFIGURATION_INVALID;
			have_readonly = true;
			if (scalar_equal(value, "true")) mount->read_only = true;
			else if (!scalar_equal(value, "false")) return INIT_CONFIGURATION_INVALID;
		}
		else return INIT_CONFIGURATION_INVALID;
	}
	return have_identity && have_path ? INIT_CONFIGURATION_OK : INIT_CONFIGURATION_INVALID;
}

static enum init_configuration_status parse_mounts(struct init_configuration* configuration, yaml_document_t* document,
                                                   const yaml_node_t* node) {
	bool root_selected = false;
	bool boot_selected = false;
	if (node == NULL || node->type != YAML_SEQUENCE_NODE) return INIT_CONFIGURATION_INVALID;
	for (yaml_node_item_t* item = node->data.sequence.items.start; item != node->data.sequence.items.top; item++) {
		if (configuration->mount_count == INIT_CONFIGURATION_MAX_MOUNTS) return INIT_CONFIGURATION_INVALID;
		struct init_mount_configuration* mount  = &configuration->mounts[configuration->mount_count];
		enum init_configuration_status   status = parse_mount(mount, document, yaml_document_get_node(document, *item));
		if (status != INIT_CONFIGURATION_OK) return status;
		for (size_t prior = 0u; prior < configuration->mount_count; prior++) {
			if (strcmp(configuration->mounts[prior].service, mount->service) == 0 ||
			    strcmp(configuration->mounts[prior].mount_point, mount->mount_point) == 0)
				return INIT_CONFIGURATION_INVALID;
		}
		configuration->mount_count++;
		if (strcmp(mount->mount_point, "/") == 0) root_selected = true;
		if (strcmp(mount->mount_point, "/boot") == 0) boot_selected = true;
	}
	return root_selected && boot_selected ? INIT_CONFIGURATION_INVALID : INIT_CONFIGURATION_OK;
}

static enum init_configuration_status parse_document(struct init_configuration* configuration,
                                                     yaml_document_t*           document) {
	yaml_node_t*                   root = yaml_document_get_root_node(document);
	enum init_configuration_status status;
	bool                           have_version    = false;
	bool                           have_text_input = false;
	bool                           have_mounts     = false;

	if (root == NULL || root->type != YAML_MAPPING_NODE) return INIT_CONFIGURATION_INVALID;
	for (yaml_node_pair_t* pair = root->data.mapping.pairs.start; pair != root->data.mapping.pairs.top; pair++) {
		yaml_node_t* key   = yaml_document_get_node(document, pair->key);
		yaml_node_t* value = yaml_document_get_node(document, pair->value);
		if (scalar_equal(key, "format_version")) {
			if (have_version || value == NULL || value->type != YAML_SCALAR_NODE ||
			    value->data.scalar.style != YAML_PLAIN_SCALAR_STYLE || !scalar_equal(value, "1"))
				return INIT_CONFIGURATION_INVALID;
			have_version = true;
		}
		else if (scalar_equal(key, "text_input")) {
			if (have_text_input) return INIT_CONFIGURATION_INVALID;
			have_text_input = true;
			status          = parse_text_input(configuration, document, value);
			if (status != INIT_CONFIGURATION_OK) return status;
		}
		else if (scalar_equal(key, "mounts")) {
			if (have_mounts) return INIT_CONFIGURATION_INVALID;
			have_mounts = true;
			status      = parse_mounts(configuration, document, value);
			if (status != INIT_CONFIGURATION_OK) return status;
		}
		else return INIT_CONFIGURATION_INVALID;
	}
	return have_version && have_text_input ? INIT_CONFIGURATION_OK : INIT_CONFIGURATION_INVALID;
}

static enum init_configuration_status parser_failure(const yaml_parser_t* parser) {
	return parser->error == YAML_MEMORY_ERROR ? INIT_CONFIGURATION_NO_MEMORY : INIT_CONFIGURATION_YAML_ERROR;
}

static bool filesystem_call_succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static bool read_file(cap_id_t file_cap, uint8_t* data, size_t size) {
	for (size_t offset = 0u; offset < size;) {
		size_t                        read = 0u;
		struct filesystem_call_result result =
			filesystem_file_read(file_cap, offset, data + offset, size - offset, &read);
		if (!filesystem_call_succeeded(result) || read == 0u || read > size - offset) return false;
		offset += read;
	}
	return true;
}

enum init_configuration_status init_configuration_load(cap_id_t vfs_cap, const char* path,
                                                       struct init_configuration* configuration) {
	struct init_configuration      loaded = {0};
	struct filesystem_node_handle  file   = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result  open_result;
	yaml_parser_t                  parser;
	yaml_document_t                document;
	uint8_t*                       data                 = NULL;
	bool                           parser_initialized   = false;
	bool                           document_initialized = false;
	enum init_configuration_status configuration_status = INIT_CONFIGURATION_IO_ERROR;
	size_t                         size;

	if (vfs_cap == CAP_ID_INVALID || path == NULL || configuration == NULL) return INIT_CONFIGURATION_INVALID;
	size = strlen(path);
	if (!vfs_absolute_path_valid(path, size)) return INIT_CONFIGURATION_INVALID;
	open_result = vfs_open(vfs_cap, path, size, CAP_READ, &file);
	if (!filesystem_call_succeeded(open_result) || file.capability == CAP_ID_INVALID ||
	    file.info.type != FILESYSTEM_NODE_FILE || file.info.size == 0u ||
	    file.info.size > INIT_CONFIGURATION_MAXIMUM_SIZE)
		goto cleanup;
	size = (size_t)file.info.size;
	data = malloc(size);
	if (data == NULL) {
		configuration_status = INIT_CONFIGURATION_NO_MEMORY;
		goto cleanup;
	}
	if (!read_file(file.capability, data, size)) goto cleanup;
	if (!yaml_parser_initialize(&parser)) {
		configuration_status = INIT_CONFIGURATION_NO_MEMORY;
		goto cleanup;
	}
	parser_initialized = true;
	yaml_parser_set_input_string(&parser, data, size);
	if (!yaml_parser_load(&parser, &document)) {
		configuration_status = parser_failure(&parser);
		goto cleanup;
	}
	document_initialized = true;
	configuration_status = parse_document(&loaded, &document);
	yaml_document_delete(&document);
	document_initialized = false;
	if (configuration_status != INIT_CONFIGURATION_OK) goto cleanup;
	if (!yaml_parser_load(&parser, &document)) {
		configuration_status = parser_failure(&parser);
		goto cleanup;
	}
	document_initialized = true;
	if (yaml_document_get_root_node(&document) != NULL) configuration_status = INIT_CONFIGURATION_INVALID;

cleanup:
	if (document_initialized) yaml_document_delete(&document);
	if (parser_initialized) yaml_parser_delete(&parser);
	free(data);
	if (file.capability != CAP_ID_INVALID && cap_drop(file.capability) != SYSCALL_STATUS_OK &&
	    configuration_status == INIT_CONFIGURATION_OK)
		configuration_status = INIT_CONFIGURATION_IO_ERROR;
	if (configuration_status == INIT_CONFIGURATION_OK) *configuration = loaded;
	return configuration_status;
}

const char* init_configuration_status_string(enum init_configuration_status status) {
	switch (status) {
	case INIT_CONFIGURATION_OK:
		return "success";
	case INIT_CONFIGURATION_IO_ERROR:
		return "I/O error";
	case INIT_CONFIGURATION_YAML_ERROR:
		return "malformed YAML";
	case INIT_CONFIGURATION_INVALID:
		return "invalid configuration";
	case INIT_CONFIGURATION_NO_MEMORY:
		return "out of memory";
	default:
		return "unknown error";
	}
}
