#include "aml.h"

#include <base/acpi.h>
#include <base/cap.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/acpi.h>
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi_host/aml.h>
#include <uacpi_host/platform.h>

#include "device.h"
#include "resource.h"

#define ACPI_COMPATIBLE_PREFIX "acpi:"
#define ACPI_COMPATIBLE_GENERIC "acpi:device"

struct aml_parse_context {
	struct acpi_device_set* devices;
	syscall_status_t        status;
};

struct acpi_aml {
	struct uacpi_host_aml_definition* definitions;
	struct acpi_device_set*           devices;
	size_t                            definition_count;
	cap_id_t                          memory_allocator_cap;
	cap_id_t                          interrupts_cap;
	cap_id_t                          io_ports_cap;
	bool                              platform_initialized;
	bool                              interpreter_initialized;
};

static bool id_string_valid(const uacpi_id_string* id) {
	return id != NULL && id->value != NULL && id->size > 1u &&
	       id->size - 1u <= DEVICE_COMPATIBLE_ID_MAX - (sizeof(ACPI_COMPATIBLE_PREFIX) - 1u) &&
	       id->value[id->size - 1u] == '\0' && memchr(id->value, '\0', id->size - 1u) == NULL;
}

static syscall_status_t add_id_compatible(struct acpi_device* device, const uacpi_id_string* id) {
	char   compatible[DEVICE_COMPATIBLE_ID_MAX + 1u];
	size_t prefix_size = sizeof(ACPI_COMPATIBLE_PREFIX) - 1u;
	size_t id_size;

	if (!id_string_valid(id)) return SYSCALL_STATUS_BAD_ARGUMENT;
	id_size = id->size - 1u;
	if (id_size > sizeof(compatible) - prefix_size) return SYSCALL_STATUS_BAD_ARGUMENT;
	memcpy(compatible, ACPI_COMPATIBLE_PREFIX, prefix_size);
	memcpy(compatible + prefix_size, id->value, id_size);
	compatible[prefix_size + id_size] = '\0';
	return acpi_device_add_compatible(device, compatible);
}

static syscall_status_t add_optional_string(const struct device_builder* builder, const char* property,
                                            const uacpi_id_string* id) {
	if (!id_string_valid(id)) return SYSCALL_STATUS_OK;
	return acpi_device_add_string(builder, property, id->value);
}

static syscall_status_t submit_device(struct aml_parse_context* context, uacpi_namespace_node* node, uacpi_u32 sta) {
	struct acpi_device*    device = NULL;
	struct device_builder* builder;
	uacpi_id_string*       hid  = NULL;
	uacpi_id_string*       uid  = NULL;
	uacpi_pnp_id_list*     cids = NULL;
	const uacpi_char*      path = NULL;
	uacpi_u64              address;
	uacpi_status           ustatus;
	syscall_status_t       status;
	size_t                 compatible_count = 0u;

	status = acpi_device_begin(context->devices, &device);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	acpi_device_set_source(device, node);
	builder = acpi_device_builder(device);

	ustatus = uacpi_eval_hid(node, &hid);
	if (ustatus == UACPI_STATUS_OK && id_string_valid(hid)) {
		status = add_id_compatible(device, hid);
		if (status == SYSCALL_STATUS_OK) compatible_count++;
	}
	else if (ustatus != UACPI_STATUS_OK && ustatus != UACPI_STATUS_NOT_FOUND) {
		printf("acpi-parser: ignoring malformed _HID: %s\n", uacpi_status_to_string(ustatus));
	}

	ustatus = uacpi_eval_cid(node, &cids);
	if (ustatus == UACPI_STATUS_OK) {
		for (uacpi_u32 index = 0u; index < cids->num_ids; index++) {
			if (!id_string_valid(&cids->ids[index])) continue;
			if (status != SYSCALL_STATUS_OK || compatible_count + 1u >= DEVICE_MAX_COMPATIBLE_IDS) continue;
			status = add_id_compatible(device, &cids->ids[index]);
			if (status == SYSCALL_STATUS_OK) compatible_count++;
		}
	}
	else if (ustatus != UACPI_STATUS_NOT_FOUND) {
		printf("acpi-parser: ignoring malformed _CID: %s\n", uacpi_status_to_string(ustatus));
	}
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_compatible(device, ACPI_COMPATIBLE_GENERIC);

	path = uacpi_namespace_node_generate_absolute_path(node);
	if (status == SYSCALL_STATUS_OK && path != NULL) {
		size_t path_size = strlen(path);
		if (path_size != 0u && path_size <= DEVICE_NAME_MAX) {
			status = device_builder_set_name(builder, path, path_size);
			if (status == SYSCALL_STATUS_OK) status = acpi_device_add_string(builder, "acpi_path", path);
		}
	}
	if (status == SYSCALL_STATUS_OK) status = add_optional_string(builder, "acpi_hid", hid);
	ustatus = uacpi_eval_uid(node, &uid);
	if (status == SYSCALL_STATUS_OK && ustatus == UACPI_STATUS_OK)
		status = add_optional_string(builder, "acpi_uid", uid);
	else if (ustatus != UACPI_STATUS_OK && ustatus != UACPI_STATUS_NOT_FOUND)
		printf("acpi-parser: ignoring malformed _UID: %s\n", uacpi_status_to_string(ustatus));
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_u64(builder, "acpi_status", sta);
	ustatus = uacpi_eval_adr(node, &address);
	if (status == SYSCALL_STATUS_OK && ustatus == UACPI_STATUS_OK)
		status = acpi_device_add_u64(builder, "acpi_address", address);
	else if (ustatus != UACPI_STATUS_OK && ustatus != UACPI_STATUS_NOT_FOUND)
		printf("acpi-parser: ignoring malformed _ADR: %s\n", uacpi_status_to_string(ustatus));
cleanup:
	uacpi_free_absolute_path(path);
	uacpi_free_pnp_id_list(cids);
	uacpi_free_id_string(uid);
	uacpi_free_id_string(hid);
	return status;
}

static uacpi_iteration_decision discover_device(void* opaque, uacpi_namespace_node* node, uacpi_u32 depth) {
	struct aml_parse_context* context = opaque;
	uacpi_u32                 sta;
	uacpi_status              status;

	(void)depth;
	if (uacpi_namespace_node_is_alias(node)) return UACPI_ITERATION_DECISION_NEXT_PEER;
	status = uacpi_eval_sta(node, &sta);
	if (status != UACPI_STATUS_OK) {
		printf("acpi-parser: _STA evaluation failed: %s\n", uacpi_status_to_string(status));
		return UACPI_ITERATION_DECISION_CONTINUE;
	}
	if ((sta & ACPI_STA_RESULT_DEVICE_PRESENT) == 0u) {
		if ((sta & ACPI_STA_RESULT_DEVICE_FUNCTIONING) != 0u) return UACPI_ITERATION_DECISION_CONTINUE;
		return UACPI_ITERATION_DECISION_NEXT_PEER;
	}
	context->status = submit_device(context, node, sta);
	if (context->status != SYSCALL_STATUS_OK) return UACPI_ITERATION_DECISION_BREAK;
	return UACPI_ITERATION_DECISION_CONTINUE;
}

static syscall_status_t read_definitions(cap_id_t provider_cap, struct uacpi_host_aml_definition** out_definitions,
                                         size_t* out_count) {
	struct uacpi_host_aml_definition* definitions = NULL;
	uint64_t                          count64;
	syscall_status_t                  status;

	*out_definitions = NULL;
	*out_count       = 0u;
	status           = acpi_aml_count(provider_cap, &count64);
	if (status != SYSCALL_STATUS_OK || count64 == 0u) return status;
	if (count64 > SIZE_MAX / sizeof(*definitions)) return SYSCALL_STATUS_FAILED;
	definitions = calloc((size_t)count64, sizeof(*definitions));
	if (definitions == NULL) return SYSCALL_STATUS_FAILED;

	for (size_t index = 0u; index < (size_t)count64; index++) {
		struct acpi_provider_aml_info_response info;
		uacpi_u8*                              aml;

		status = acpi_aml_info(provider_cap, index, &info);
		if (status != SYSCALL_STATUS_OK) goto fail;
		if (info.kind != (index == 0u ? ACPI_AML_DEFINITION_DSDT : ACPI_AML_DEFINITION_SSDT)) {
			status = SYSCALL_STATUS_FAILED;
			goto fail;
		}
		if (info.aml_size > SIZE_MAX || info.aml_size > UINT32_MAX) {
			status = SYSCALL_STATUS_FAILED;
			goto fail;
		}
		aml = malloc(info.aml_size == 0u ? 1u : (size_t)info.aml_size);
		if (aml == NULL) {
			status = SYSCALL_STATUS_FAILED;
			goto fail;
		}
		definitions[index] = (struct uacpi_host_aml_definition){
			.aml          = {.bytes = aml, .length = (size_t)info.aml_size},
			.oem_revision = info.oem_revision,
			.kind =
				info.kind == ACPI_AML_DEFINITION_DSDT ? UACPI_HOST_AML_DEFINITION_DSDT : UACPI_HOST_AML_DEFINITION_SSDT,
			.revision = info.revision,
		};
		memcpy(definitions[index].oem_id, info.oem_id, sizeof(info.oem_id));
		memcpy(definitions[index].oem_table_id, info.oem_table_id, sizeof(info.oem_table_id));
		for (uint64_t offset = 0u; offset < info.aml_size;) {
			size_t chunk = info.aml_size - offset > CAP_MAX_RESPONSE_SIZE ? CAP_MAX_RESPONSE_SIZE
			                                                              : (size_t)(info.aml_size - offset);
			status       = acpi_aml_read(provider_cap, index, offset, aml + offset, chunk);
			if (status != SYSCALL_STATUS_OK) goto fail;
			offset += chunk;
		}
	}
	*out_definitions = definitions;
	*out_count       = (size_t)count64;
	return SYSCALL_STATUS_OK;

fail:
	for (size_t index = 0u; index < (size_t)count64; index++) free(definitions[index].aml.data);
	free(definitions);
	return status;
}

void acpi_aml_deinitialize(struct acpi_aml* aml) {
	if (aml == NULL) return;
	for (struct acpi_device* device = acpi_device_first(aml->devices); device != NULL;
	     device                     = acpi_device_next(device))
		acpi_device_set_source(device, NULL);
	if (aml->interpreter_initialized) uacpi_host_reset();
	if (aml->platform_initialized) uacpi_host_platform_deinitialize();
	for (size_t index = 0u; index < aml->definition_count; index++) free(aml->definitions[index].aml.data);
	free(aml->definitions);
	free(aml);
}

syscall_status_t acpi_aml_initialize(cap_id_t provider_cap, struct acpi_device_set* devices,
                                     cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                                     struct acpi_aml** out_aml) {
	struct acpi_aml*         aml;
	struct aml_parse_context context;
	uacpi_status             ustatus;
	syscall_status_t         status;

	if (provider_cap == CAP_ID_INVALID || devices == NULL || memory_allocator_cap == CAP_ID_INVALID || out_aml == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_aml = NULL;
	aml      = calloc(1u, sizeof(*aml));
	if (aml == NULL) return SYSCALL_STATUS_FAILED;
	aml->devices              = devices;
	aml->memory_allocator_cap = memory_allocator_cap;
	aml->interrupts_cap       = interrupts_cap;
	aml->io_ports_cap         = io_ports_cap;
	status                    = read_definitions(provider_cap, &aml->definitions, &aml->definition_count);
	if (status != SYSCALL_STATUS_OK || aml->definition_count == 0u) goto fail;
	if (!uacpi_host_platform_initialize(memory_allocator_cap, io_ports_cap)) {
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	aml->platform_initialized = true;
	ustatus                   = uacpi_host_initialize(UACPI_FLAG_NO_ACPI_MODE, aml->definitions, aml->definition_count);
	if (ustatus != UACPI_STATUS_OK) {
		printf("acpi-parser: uACPI initialization failed: %s\n", uacpi_status_to_string(ustatus));
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	aml->interpreter_initialized = true;
	ustatus                      = uacpi_host_namespace_load();
	if (ustatus != UACPI_STATUS_OK) {
		printf("acpi-parser: AML namespace load failed: %s\n", uacpi_status_to_string(ustatus));
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	ustatus = uacpi_namespace_initialize();
	if (ustatus != UACPI_STATUS_OK) {
		printf("acpi-parser: AML namespace initialization failed: %s\n", uacpi_status_to_string(ustatus));
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	context = (struct aml_parse_context){
		.devices = devices,
		.status  = SYSCALL_STATUS_OK,
	};
	ustatus = uacpi_namespace_for_each_child(
		uacpi_namespace_root(), discover_device, NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &context);
	if (ustatus != UACPI_STATUS_OK) {
		printf("acpi-parser: AML namespace walk failed: %s\n", uacpi_status_to_string(ustatus));
		status = SYSCALL_STATUS_FAILED;
	}
	else status = context.status;
	if (status != SYSCALL_STATUS_OK) goto fail;
	*out_aml = aml;
	return SYSCALL_STATUS_OK;

fail:
	acpi_aml_deinitialize(aml);
	return status;
}

syscall_status_t acpi_aml_add_resources(struct acpi_aml* aml) {
	if (aml == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	for (struct acpi_device* device = acpi_device_first(aml->devices); device != NULL;
	     device                     = acpi_device_next(device)) {
		uacpi_namespace_node* node = acpi_device_source(device);

		if (node == NULL) continue;
		syscall_status_t status =
			acpi_resources_add_current(device, node, aml->memory_allocator_cap, aml->interrupts_cap, aml->io_ports_cap);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_aml_evaluate_u64(const struct acpi_device* device, const char* path, uint64_t* out_value) {
	uacpi_namespace_node* node;
	uacpi_u64             value;
	uacpi_status          status;

	if (device == NULL || path == NULL || path[0] == '\0' || out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	node = acpi_device_source(device);
	if (node == NULL) return SYSCALL_STATUS_UNAVAILABLE;
	status = uacpi_eval_simple_integer(node, path, &value);
	if (status == UACPI_STATUS_NOT_FOUND) return SYSCALL_STATUS_UNAVAILABLE;
	if (status != UACPI_STATUS_OK) return SYSCALL_STATUS_FAILED;
	*out_value = value;
	return SYSCALL_STATUS_OK;
}
