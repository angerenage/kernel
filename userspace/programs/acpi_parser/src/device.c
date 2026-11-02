#include "device.h"

#include <base/cap.h>
#include <base/process.h>
#include <protocol/device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

#if defined(__x86_64__)
#include <base/io_port.h>
#include <system/io_port.h>
#endif

struct acpi_compatible {
	struct acpi_compatible* next;
	size_t                  size;
	char                    value[];
};

enum acpi_staged_resource_kind { ACPI_STAGED_MMIO, ACPI_STAGED_IO, ACPI_STAGED_INTERRUPT };
struct acpi_staged_resource {
	struct acpi_staged_resource*   next;
	char*                          name;
	enum acpi_staged_resource_kind kind;
	uintptr_t                      address;
	size_t                         size;
	uint32_t                       source;
	enum interrupt_trigger         trigger;
	enum interrupt_polarity        polarity;
};

struct acpi_device {
	struct acpi_device*          next;
	struct device_builder        builder;
	struct acpi_compatible*      compatibles;
	void*                        source;
	struct acpi_staged_resource* resources;
	struct acpi_staged_resource* resources_tail;
};

syscall_status_t acpi_device_set_initialize(struct acpi_device_set* set, cap_id_t root_cap) {
	if (set == NULL || root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	*set = (struct acpi_device_set){.root_cap = root_cap};
	return SYSCALL_STATUS_OK;
}

void acpi_device_set_deinitialize(struct acpi_device_set* set) {
	if (set == NULL) return;
	while (set->head != NULL) {
		struct acpi_device* device = set->head;

		set->head = device->next;
		if (device->builder.capability != CAP_ID_INVALID) (void)device_builder_abort(&device->builder);
		while (device->compatibles != NULL) {
			struct acpi_compatible* compatible = device->compatibles;

			device->compatibles = compatible->next;
			free(compatible);
		}
		while (device->resources != NULL) {
			struct acpi_staged_resource* resource = device->resources;
			device->resources                     = resource->next;
			free(resource->name);
			free(resource);
		}
		free(device);
	}
	*set = (struct acpi_device_set){.root_cap = CAP_ID_INVALID};
}

syscall_status_t acpi_device_set_commit(struct acpi_device_set* set, size_t* out_device_count) {
	size_t count = 0u;

	if (set == NULL || out_device_count == NULL || set->root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_device_count = 0u;
	for (struct acpi_device* device = set->head; device != NULL; device = device->next) {
		if (count == SIZE_MAX) return SYSCALL_STATUS_FAILED;
		syscall_status_t status = device_builder_commit(&device->builder);
		if (status != SYSCALL_STATUS_OK) return status;
		count++;
	}
	*out_device_count = count;
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_device_begin(struct acpi_device_set* set, struct acpi_device** out_device) {
	struct acpi_device* device;
	syscall_status_t    status;

	if (set == NULL || out_device == NULL || set->root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_device = NULL;
	device      = calloc(1u, sizeof(*device));
	if (device == NULL) return SYSCALL_STATUS_FAILED;
	device->builder = (struct device_builder){.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	status          = device_builder_begin_root(set->root_cap, &device->builder);
	if (status != SYSCALL_STATUS_OK) {
		free(device);
		return status;
	}
	if (set->tail == NULL) set->head = device;
	else set->tail->next = device;
	set->tail   = device;
	*out_device = device;
	return SYSCALL_STATUS_OK;
}

struct acpi_device* acpi_device_find_compatible(const struct acpi_device_set* set, const char* compatible) {
	if (set == NULL || compatible == NULL) return NULL;
	for (struct acpi_device* device = set->head; device != NULL; device = device->next)
		if (acpi_device_has_compatible(device, compatible)) return device;
	return NULL;
}

struct acpi_device* acpi_device_first(const struct acpi_device_set* set) {
	return set == NULL ? NULL : set->head;
}

struct acpi_device* acpi_device_next(const struct acpi_device* device) {
	return device == NULL ? NULL : device->next;
}

bool acpi_device_has_compatible(const struct acpi_device* device, const char* compatible) {
	size_t size;

	if (device == NULL || compatible == NULL) return false;
	size = strlen(compatible);
	for (const struct acpi_compatible* id = device->compatibles; id != NULL; id = id->next)
		if (id->size == size && memcmp(id->value, compatible, size) == 0) return true;
	return false;
}

struct device_builder* acpi_device_builder(struct acpi_device* device) {
	return device == NULL ? NULL : &device->builder;
}

syscall_status_t acpi_device_add_compatible(struct acpi_device* device, const char* compatible) {
	struct acpi_compatible* id;
	struct acpi_compatible* tail;
	size_t                  size;
	syscall_status_t        status;

	if (device == NULL || compatible == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	size = strlen(compatible);
	for (id = device->compatibles; id != NULL; id = id->next)
		if (id->size == size && memcmp(id->value, compatible, size) == 0) return SYSCALL_STATUS_OK;
	if (size > SIZE_MAX - sizeof(*id)) return SYSCALL_STATUS_FAILED;
	id = malloc(sizeof(*id) + size);
	if (id == NULL) return SYSCALL_STATUS_FAILED;
	status = device_builder_add_compatible(&device->builder, compatible, size);
	if (status != SYSCALL_STATUS_OK) {
		free(id);
		return status;
	}
	id->next = NULL;
	id->size = size;
	memcpy(id->value, compatible, size);
	if (device->compatibles == NULL) device->compatibles = id;
	else {
		for (tail = device->compatibles; tail->next != NULL; tail = tail->next) {
		}
		tail->next = id;
	}
	return SYSCALL_STATUS_OK;
}

void acpi_device_set_source(struct acpi_device* device, void* source) {
	if (device != NULL) device->source = source;
}

void* acpi_device_source(const struct acpi_device* device) {
	return device == NULL ? NULL : device->source;
}

syscall_status_t acpi_device_add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t          encoded[sizeof(value)];
	syscall_status_t status;

	if (name == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	for (size_t index = 0u; index < sizeof(encoded); index++) encoded[index] = (uint8_t)(value >> (index * 8u));
	status = device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, encoded, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

syscall_status_t acpi_device_add_string(const struct device_builder* builder, const char* name, const char* value) {
	size_t           value_size;
	syscall_status_t status;

	if (name == NULL || value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	value_size = strlen(value);
	status = device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UTF8_STRING, 1u, value_size);
	if (status == SYSCALL_STATUS_OK && value_size != 0u)
		status = device_builder_append_property(builder, 0u, value, value_size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static syscall_status_t stage_resource(struct acpi_device* device, const char* name,
                                       enum acpi_staged_resource_kind kind, uintptr_t address, size_t size,
                                       uint32_t source, enum interrupt_trigger trigger,
                                       enum interrupt_polarity polarity) {
	struct acpi_staged_resource* resource;
	size_t                       length;
	if (device == NULL || name == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	length = strlen(name);
	if (length == 0u || length > DEVICE_IDENTIFIER_MAX) return SYSCALL_STATUS_BAD_ARGUMENT;
	resource = calloc(1u, sizeof(*resource));
	if (resource == NULL) return SYSCALL_STATUS_FAILED;
	resource->name = malloc(length + 1u);
	if (resource->name == NULL) {
		free(resource);
		return SYSCALL_STATUS_FAILED;
	}
	memcpy(resource->name, name, length + 1u);
	resource->kind     = kind;
	resource->address  = address;
	resource->size     = size;
	resource->source   = source;
	resource->trigger  = trigger;
	resource->polarity = polarity;
	if (device->resources_tail == NULL) device->resources = resource;
	else device->resources_tail->next = resource;
	device->resources_tail = resource;
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_device_stage_mmio(struct acpi_device* device, const char* name, uintptr_t address, size_t size) {
	if (size == 0u || address > UINTPTR_MAX - (size - 1u)) return SYSCALL_STATUS_BAD_ARGUMENT;
	return stage_resource(device, name, ACPI_STAGED_MMIO, address, size, 0u, 0, 0);
}

syscall_status_t acpi_device_stage_io(struct acpi_device* device, const char* name, uint32_t address, uint32_t size) {
	if (size == 0u || (uint64_t)address + size > (uint64_t)UINT32_MAX + 1u) return SYSCALL_STATUS_BAD_ARGUMENT;
	return stage_resource(device, name, ACPI_STAGED_IO, address, size, 0u, 0, 0);
}

syscall_status_t acpi_device_stage_interrupt(struct acpi_device* device, const char* name, uint32_t source,
                                             enum interrupt_trigger trigger, enum interrupt_polarity polarity) {
	return stage_resource(device, name, ACPI_STAGED_INTERRUPT, 0u, 0u, source, trigger, polarity);
}

static syscall_status_t publish_one(struct acpi_device* device, const struct acpi_staged_resource* resource,
                                    cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap) {
	struct device_builder* builder   = &device->builder;
	size_t                 name_size = strlen(resource->name);
	switch (resource->kind) {
	case ACPI_STAGED_MMIO:
		return device_builder_add_mmio_resource(
			builder, memory_allocator_cap, resource->name, name_size, resource->address, resource->size);
	case ACPI_STAGED_INTERRUPT:
		if (interrupts_cap == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
		return device_builder_add_interrupt_resource(builder,
		                                             interrupts_cap,
		                                             resource->name,
		                                             name_size,
		                                             INTERRUPT_SOURCE_CONTROLLER_PLATFORM,
		                                             resource->source,
		                                             resource->trigger,
		                                             resource->polarity);
	case ACPI_STAGED_IO:
#if defined(__x86_64__)
	{
		static const cap_rights_t driver_rights = CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP;
		struct io_port_info       range;
		cap_id_t                  cap = CAP_ID_INVALID;
		syscall_status_t          status;
		if (io_ports_cap == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
		status = io_port_info(io_ports_cap, &range);
		if (status != SYSCALL_STATUS_OK) return status;
		if (resource->address < range.base || resource->address - range.base > range.count ||
		    resource->size > range.count - (resource->address - range.base))
			return SYSCALL_STATUS_OK;
		status = io_port_derive(io_ports_cap,
		                        (uint32_t)(resource->address - range.base),
		                        (uint32_t)resource->size,
		                        driver_rights | CAP_DELEGATE,
		                        &cap);
		if (status == SYSCALL_STATUS_OK)
			status = device_builder_add_resource(builder, resource->name, name_size, cap, driver_rights);
		if (cap != CAP_ID_INVALID) {
			syscall_status_t drop_status = cap_drop(cap);
			if (status == SYSCALL_STATUS_OK) status = drop_status;
		}
		return status;
	}
#else
		(void)io_ports_cap;
		return SYSCALL_STATUS_OK;
#endif
	}
	return SYSCALL_STATUS_BAD_ARGUMENT;
}

syscall_status_t acpi_device_set_publish_resources(struct acpi_device_set* set, cap_id_t memory_allocator_cap,
                                                   cap_id_t interrupts_cap, cap_id_t io_ports_cap) {
	if (set == NULL || memory_allocator_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	size_t failed = 0u;
	for (struct acpi_device* device = set->head; device != NULL; device = device->next) {
		for (struct acpi_staged_resource* resource = device->resources; resource != NULL; resource = resource->next) {
			syscall_status_t status = publish_one(device, resource, memory_allocator_cap, interrupts_cap, io_ports_cap);
			if (status != SYSCALL_STATUS_OK) {
				if (failed < 4u)
					printf("acpi: resource skipped name=%s kind=%u status=%u\n",
					       resource->name,
					       (unsigned)resource->kind,
					       (unsigned)status);
				failed++;
			}
		}
	}
	if (failed != 0u) printf("acpi: %zu resource(s) skipped during publication\n", failed);
	return SYSCALL_STATUS_OK;
}
