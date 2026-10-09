#include "device.h"

#include <protocol/device.h>
#include <string.h>

syscall_status_t acpi_device_add_compatible(const struct device_builder* builder, const char* compatible) {
	if (compatible == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	return device_builder_add_compatible(builder, compatible, strlen(compatible));
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
