#pragma once

#include <base/syscall.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct acpi_device;

struct acpi_device_set {
	cap_id_t            root_cap;
	struct acpi_device* head;
	struct acpi_device* tail;
};

/* Stage ACPI devices so AML objects can be enriched by fixed-table observations before publication. */
syscall_status_t acpi_device_set_initialize(struct acpi_device_set* set, cap_id_t root_cap);

/* Deinitialize the ACPI device set and free all associated resources. */
void acpi_device_set_deinitialize(struct acpi_device_set* set);

/* Commit all staged ACPI devices to the device manager and return the number of committed devices. */
syscall_status_t acpi_device_set_commit(struct acpi_device_set* set, size_t* out_device_count);

/* Start a staged root device and retrieve an existing one by an exact compatible ID. */
syscall_status_t acpi_device_begin(struct acpi_device_set* set, struct acpi_device** out_device);

/* Find an existing ACPI device by an exact compatible ID. */
struct acpi_device* acpi_device_find_compatible(const struct acpi_device_set* set, const char* compatible);

/* Iterate staged devices and inspect their compatible IDs. */
struct acpi_device* acpi_device_first(const struct acpi_device_set* set);

/* Get the next staged ACPI device in the set. */
struct acpi_device* acpi_device_next(const struct acpi_device* device);

/* Check if an ACPI device has a specific compatible ID. */
bool acpi_device_has_compatible(const struct acpi_device* device, const char* compatible);

/* Get a builder for the specified ACPI device. */
struct device_builder* acpi_device_builder(struct acpi_device* device);

/* Append one unique NUL-free compatible ID without its C terminator. */
syscall_status_t acpi_device_add_compatible(struct acpi_device* device, const char* compatible);

/* Associate parser-private source data with a staged device. */
void acpi_device_set_source(struct acpi_device* device, void* source);

/* Retrieve the parser-private source data associated with a staged device. */
void* acpi_device_source(const struct acpi_device* device);

/* Append one unsigned 64-bit property in canonical little-endian form. */
syscall_status_t acpi_device_add_u64(const struct device_builder* builder, const char* name, uint64_t value);

/* Append one UTF-8 string property without its C terminator. */
syscall_status_t acpi_device_add_string(const struct device_builder* builder, const char* name, const char* value);
