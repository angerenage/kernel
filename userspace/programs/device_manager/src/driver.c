#include "driver.h"

#include <base/cap.h>
#include <protocol/device.h>
#include <protocol/vfs.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <yaml.h>

static bool string_equal(const struct dm_string* string, const void* bytes, size_t size) {
	return string->size == size && (size == 0u || memcmp(string->data, bytes, size) == 0);
}

static bool scalar_equal(const yaml_node_t* node, const char* expected) {
	size_t expected_size = strlen(expected);
	return node != NULL && node->type == YAML_SCALAR_NODE && node->data.scalar.length == expected_size &&
	       memcmp(node->data.scalar.value, expected, expected_size) == 0;
}

static bool string_copy(struct dm_string* out, const void* bytes, size_t size) {
	char* copy;

	if (size == 0u) {
		*out = (struct dm_string){0};
		return true;
	}
	copy = malloc(size);
	if (copy == NULL) return false;
	memcpy(copy, bytes, size);
	*out = (struct dm_string){.data = copy, .size = size};
	return true;
}

static void driver_deinit(struct dm_driver* driver) {
	if (driver == NULL) return;
	if (driver->executable_cap != CAP_ID_INVALID) (void)cap_drop(driver->executable_cap);
	free(driver->name.data);
	free(driver->executable.data);
	for (size_t index = 0u; index < driver->compatible_count; index++) free(driver->compatible_ids[index].data);
	free(driver->compatible_ids);
	free(driver);
}

void dm_driver_catalog_init(struct dm_driver_catalog* catalog) {
	if (catalog != NULL) *catalog = (struct dm_driver_catalog){0};
}

void dm_driver_catalog_deinit(struct dm_driver_catalog* catalog) {
	struct dm_driver* driver;

	if (catalog == NULL) return;
	driver = catalog->drivers;
	while (driver != NULL) {
		struct dm_driver* next = driver->next;
		driver_deinit(driver);
		driver = next;
	}
	*catalog = (struct dm_driver_catalog){0};
}

const struct dm_driver* dm_driver_catalog_find_compatible(const struct dm_driver_catalog* catalog,
                                                          const void* compatible_id, size_t compatible_id_size) {
	if (catalog == NULL || compatible_id == NULL || compatible_id_size == 0u) return NULL;
	for (const struct dm_driver* driver = catalog->drivers; driver != NULL; driver = driver->next) {
		for (size_t index = 0u; index < driver->compatible_count; index++) {
			if (string_equal(&driver->compatible_ids[index], compatible_id, compatible_id_size)) return driver;
		}
	}
	return NULL;
}

static enum dm_driver_catalog_status append_compatible(struct dm_driver_catalog* catalog, struct dm_driver* driver,
                                                       const yaml_node_t* node) {
	struct dm_string* resized;
	struct dm_string  compatible = {0};
	size_t            size;

	if (node == NULL || node->type != YAML_SCALAR_NODE) return DM_DRIVER_CATALOG_INVALID;
	size = node->data.scalar.length;
	if (size == 0u || size > DEVICE_COMPATIBLE_ID_MAX || !dm_utf8_valid(node->data.scalar.value, size))
		return DM_DRIVER_CATALOG_INVALID;
	for (size_t index = 0u; index < driver->compatible_count; index++) {
		if (string_equal(&driver->compatible_ids[index], node->data.scalar.value, size))
			return DM_DRIVER_CATALOG_INVALID;
	}
	if (dm_driver_catalog_find_compatible(catalog, node->data.scalar.value, size) != NULL)
		return DM_DRIVER_CATALOG_INVALID;
	if (driver->compatible_count == SIZE_MAX ||
	    driver->compatible_count + 1u > SIZE_MAX / sizeof(*driver->compatible_ids))
		return DM_DRIVER_CATALOG_NO_MEMORY;
	if (!string_copy(&compatible, node->data.scalar.value, size)) return DM_DRIVER_CATALOG_NO_MEMORY;
	resized = realloc(driver->compatible_ids, (driver->compatible_count + 1u) * sizeof(*resized));
	if (resized == NULL) {
		free(compatible.data);
		return DM_DRIVER_CATALOG_NO_MEMORY;
	}
	driver->compatible_ids                             = resized;
	driver->compatible_ids[driver->compatible_count++] = compatible;
	return DM_DRIVER_CATALOG_OK;
}

static enum dm_driver_catalog_status parse_compatible_ids(struct dm_driver_catalog* catalog, struct dm_driver* driver,
                                                          yaml_document_t* document, const yaml_node_t* node) {
	enum dm_driver_catalog_status status;

	if (node == NULL || node->type != YAML_SEQUENCE_NODE ||
	    node->data.sequence.items.start == node->data.sequence.items.top)
		return DM_DRIVER_CATALOG_INVALID;
	for (yaml_node_item_t* item = node->data.sequence.items.start; item != node->data.sequence.items.top; item++) {
		status = append_compatible(catalog, driver, yaml_document_get_node(document, *item));
		if (status != DM_DRIVER_CATALOG_OK) return status;
	}
	return DM_DRIVER_CATALOG_OK;
}

static enum dm_driver_catalog_status parse_executable(struct dm_driver* driver, const yaml_node_t* node) {
	size_t size;

	if (node == NULL || node->type != YAML_SCALAR_NODE) return DM_DRIVER_CATALOG_INVALID;
	size = node->data.scalar.length;
	if (size == 0u || size > CAP_MAX_REQUEST_SIZE - sizeof(struct vfs_open_request) ||
	    !dm_utf8_valid(node->data.scalar.value, size) || !vfs_absolute_path_valid(node->data.scalar.value, size))
		return DM_DRIVER_CATALOG_INVALID;
	if (!string_copy(&driver->executable, node->data.scalar.value, size)) return DM_DRIVER_CATALOG_NO_MEMORY;
	return DM_DRIVER_CATALOG_OK;
}

static enum dm_driver_catalog_status parse_capabilities(struct dm_driver* driver, yaml_document_t* document,
                                                        const yaml_node_t* node) {
	if (node == NULL || node->type != YAML_SEQUENCE_NODE) return DM_DRIVER_CATALOG_INVALID;
	for (yaml_node_item_t* item = node->data.sequence.items.start; item != node->data.sequence.items.top; item++) {
		const yaml_node_t* capability = yaml_document_get_node(document, *item);
		uint32_t           flag;

		if (scalar_equal(capability, "memory_allocator")) flag = DM_DRIVER_CAPABILITY_MEMORY_ALLOCATOR;
		else if (scalar_equal(capability, "dma")) flag = DM_DRIVER_CAPABILITY_DMA;
		else if (scalar_equal(capability, "interrupts")) flag = DM_DRIVER_CAPABILITY_INTERRUPTS;
		else if (scalar_equal(capability, "io_ports")) flag = DM_DRIVER_CAPABILITY_IO_PORTS;
		else return DM_DRIVER_CATALOG_INVALID;
		if ((driver->root_capabilities & flag) != 0u) return DM_DRIVER_CATALOG_INVALID;
		driver->root_capabilities |= flag;
	}
	return DM_DRIVER_CATALOG_OK;
}

static enum dm_driver_catalog_status parse_driver(struct dm_driver_catalog* catalog, yaml_document_t* document,
                                                  const yaml_node_t* name_node, const yaml_node_t* declaration) {
	struct dm_driver*             driver;
	enum dm_driver_catalog_status status              = DM_DRIVER_CATALOG_INVALID;
	bool                          have_executable     = false;
	bool                          have_compatible_ids = false;
	bool                          have_capabilities   = false;

	if (name_node == NULL || name_node->type != YAML_SCALAR_NODE ||
	    !dm_identifier_valid(name_node->data.scalar.value, name_node->data.scalar.length) || declaration == NULL ||
	    declaration->type != YAML_MAPPING_NODE)
		return DM_DRIVER_CATALOG_INVALID;
	for (const struct dm_driver* existing = catalog->drivers; existing != NULL; existing = existing->next) {
		if (string_equal(&existing->name, name_node->data.scalar.value, name_node->data.scalar.length))
			return DM_DRIVER_CATALOG_INVALID;
	}
	driver = calloc(1u, sizeof(*driver));
	if (driver == NULL) return DM_DRIVER_CATALOG_NO_MEMORY;
	driver->executable_cap = CAP_ID_INVALID;
	if (!string_copy(&driver->name, name_node->data.scalar.value, name_node->data.scalar.length)) {
		status = DM_DRIVER_CATALOG_NO_MEMORY;
		goto fail;
	}
	for (yaml_node_pair_t* pair = declaration->data.mapping.pairs.start; pair != declaration->data.mapping.pairs.top;
	     pair++) {
		yaml_node_t* key   = yaml_document_get_node(document, pair->key);
		yaml_node_t* value = yaml_document_get_node(document, pair->value);
		status             = DM_DRIVER_CATALOG_INVALID;
		if (scalar_equal(key, "executable")) {
			if (have_executable) goto fail;
			have_executable = true;
			status          = parse_executable(driver, value);
		}
		else if (scalar_equal(key, "compatible_ids")) {
			if (have_compatible_ids) goto fail;
			have_compatible_ids = true;
			status              = parse_compatible_ids(catalog, driver, document, value);
		}
		else if (scalar_equal(key, "root_capabilities")) {
			if (have_capabilities) goto fail;
			have_capabilities = true;
			status            = parse_capabilities(driver, document, value);
		}
		else goto fail;
		if (status != DM_DRIVER_CATALOG_OK) goto fail;
	}
	if (!have_executable || !have_compatible_ids) {
		status = DM_DRIVER_CATALOG_INVALID;
		goto fail;
	}
	if (catalog->driver_count == SIZE_MAX) {
		status = DM_DRIVER_CATALOG_NO_MEMORY;
		goto fail;
	}
	if (catalog->drivers_tail == NULL) catalog->drivers = driver;
	else catalog->drivers_tail->next = driver;
	catalog->drivers_tail = driver;
	catalog->driver_count++;
	return DM_DRIVER_CATALOG_OK;

fail:
	driver_deinit(driver);
	return status;
}

static enum dm_driver_catalog_status parse_drivers(struct dm_driver_catalog* catalog, yaml_document_t* document,
                                                   const yaml_node_t* node) {
	enum dm_driver_catalog_status status;

	if (node == NULL || node->type != YAML_MAPPING_NODE) return DM_DRIVER_CATALOG_INVALID;
	for (yaml_node_pair_t* pair = node->data.mapping.pairs.start; pair != node->data.mapping.pairs.top; pair++) {
		status = parse_driver(catalog,
		                      document,
		                      yaml_document_get_node(document, pair->key),
		                      yaml_document_get_node(document, pair->value));
		if (status != DM_DRIVER_CATALOG_OK) return status;
	}
	return DM_DRIVER_CATALOG_OK;
}

static enum dm_driver_catalog_status parse_document(struct dm_driver_catalog* catalog, yaml_document_t* document) {
	yaml_node_t*                  root         = yaml_document_get_root_node(document);
	enum dm_driver_catalog_status status       = DM_DRIVER_CATALOG_INVALID;
	bool                          have_version = false;
	bool                          have_drivers = false;

	if (root == NULL || root->type != YAML_MAPPING_NODE) return DM_DRIVER_CATALOG_INVALID;
	for (yaml_node_pair_t* pair = root->data.mapping.pairs.start; pair != root->data.mapping.pairs.top; pair++) {
		yaml_node_t* key   = yaml_document_get_node(document, pair->key);
		yaml_node_t* value = yaml_document_get_node(document, pair->value);
		if (scalar_equal(key, "format_version")) {
			if (have_version || value == NULL || value->type != YAML_SCALAR_NODE ||
			    value->data.scalar.style != YAML_PLAIN_SCALAR_STYLE || !scalar_equal(value, "1"))
				return DM_DRIVER_CATALOG_INVALID;
			have_version = true;
		}
		else if (scalar_equal(key, "drivers")) {
			if (have_drivers) return DM_DRIVER_CATALOG_INVALID;
			have_drivers = true;
			status       = parse_drivers(catalog, document, value);
			if (status != DM_DRIVER_CATALOG_OK) return status;
		}
		else return DM_DRIVER_CATALOG_INVALID;
	}
	return have_version && have_drivers ? DM_DRIVER_CATALOG_OK : DM_DRIVER_CATALOG_INVALID;
}

static enum dm_driver_catalog_status parser_failure(const yaml_parser_t* parser, FILE* stream) {
	if (parser->error == YAML_MEMORY_ERROR) return DM_DRIVER_CATALOG_NO_MEMORY;
	if (ferror(stream)) return DM_DRIVER_CATALOG_IO_ERROR;
	return DM_DRIVER_CATALOG_YAML_ERROR;
}

enum dm_driver_catalog_status dm_driver_catalog_load(struct dm_driver_catalog* catalog, const char* path) {
	struct dm_driver_catalog      loaded;
	yaml_parser_t                 parser;
	yaml_document_t               document;
	enum dm_driver_catalog_status status               = DM_DRIVER_CATALOG_IO_ERROR;
	FILE*                         stream               = NULL;
	bool                          parser_initialized   = false;
	bool                          document_initialized = false;

	if (catalog == NULL || path == NULL) return DM_DRIVER_CATALOG_INVALID;
	dm_driver_catalog_init(&loaded);
	stream = fopen(path, "r");
	if (stream == NULL) goto cleanup;
	if (!yaml_parser_initialize(&parser)) {
		status = DM_DRIVER_CATALOG_NO_MEMORY;
		goto cleanup;
	}
	parser_initialized = true;
	yaml_parser_set_input_file(&parser, stream);
	if (!yaml_parser_load(&parser, &document)) {
		status = parser_failure(&parser, stream);
		goto cleanup;
	}
	document_initialized = true;
	status               = parse_document(&loaded, &document);
	yaml_document_delete(&document);
	document_initialized = false;
	if (status != DM_DRIVER_CATALOG_OK) goto cleanup;
	if (!yaml_parser_load(&parser, &document)) {
		status = parser_failure(&parser, stream);
		goto cleanup;
	}
	document_initialized = true;
	if (yaml_document_get_root_node(&document) != NULL) status = DM_DRIVER_CATALOG_INVALID;

cleanup:
	if (document_initialized) yaml_document_delete(&document);
	if (parser_initialized) yaml_parser_delete(&parser);
	if (stream != NULL && fclose(stream) != 0 && status == DM_DRIVER_CATALOG_OK) status = DM_DRIVER_CATALOG_IO_ERROR;
	if (status != DM_DRIVER_CATALOG_OK) {
		dm_driver_catalog_deinit(&loaded);
		return status;
	}
	dm_driver_catalog_deinit(catalog);
	*catalog = loaded;
	return DM_DRIVER_CATALOG_OK;
}

const char* dm_driver_catalog_status_string(enum dm_driver_catalog_status status) {
	switch (status) {
	case DM_DRIVER_CATALOG_OK:
		return "success";
	case DM_DRIVER_CATALOG_IO_ERROR:
		return "I/O error";
	case DM_DRIVER_CATALOG_YAML_ERROR:
		return "malformed YAML";
	case DM_DRIVER_CATALOG_INVALID:
		return "invalid catalog";
	case DM_DRIVER_CATALOG_NO_MEMORY:
		return "out of memory";
	default:
		return "unknown error";
	}
}
