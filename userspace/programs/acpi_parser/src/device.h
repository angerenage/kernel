#pragma once

#include <base/syscall.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>

/* Append one NUL-free compatible ID without its C terminator. */
syscall_status_t acpi_device_add_compatible(const struct device_builder* builder, const char* compatible);

/* Append one unsigned 64-bit property in canonical little-endian form. */
syscall_status_t acpi_device_add_u64(const struct device_builder* builder, const char* name, uint64_t value);

/* Append one UTF-8 string property without its C terminator. */
syscall_status_t acpi_device_add_string(const struct device_builder* builder, const char* name, const char* value);
