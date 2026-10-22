#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <uacpi/namespace.h>

#include "device.h"

/* Translate the generic capabilities described by one device's _CRS resource template. */
syscall_status_t acpi_resources_add_current(struct acpi_device* device, uacpi_namespace_node* node,
                                            cap_id_t memory_allocator_cap, cap_id_t interrupts_cap,
                                            cap_id_t io_ports_cap);
