#pragma once

#include <base/cap.h>
#include <base/syscall.h>

#include "device.h"

struct acpi_aml;

/* Load sanitized AML and stage each present Device object while retaining its namespace for table enrichment. */
syscall_status_t acpi_aml_initialize(cap_id_t provider_cap, struct acpi_device_set* devices,
                                     cap_id_t memory_allocator_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                                     struct acpi_aml** out_aml);

/* Materialize generic _CRS resources after fixed tables have enriched the staged AML devices. */
syscall_status_t acpi_aml_add_resources(struct acpi_aml* aml);

/* Deinitialize the AML context and free all associated resources. */
void acpi_aml_deinitialize(struct acpi_aml* aml);

/* Evaluate an integer object relative to a staged AML device. */
syscall_status_t acpi_aml_evaluate_u64(const struct acpi_device* device, const char* path, uint64_t* out_value);
