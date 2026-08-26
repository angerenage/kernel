#pragma once

#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/process.h>
#include <stdbool.h>

/* Create the singleton RSDP, DTB, and framebuffer capability objects. */
void kernel_capability_boot_resources_init(void);

/* Report whether a validated immutable boot-data resource is available. */
bool kernel_capability_boot_data_available(enum kernel_resource_type type);

/* Grant one immutable boot-data capability (RSDP or DTB). */
cap_id_t kernel_capability_boot_data_grant(enum kernel_resource_type type, process_id_t recipient);

/* Report whether a validated primary framebuffer is available. */
bool kernel_capability_framebuffer_available(void);

/* Grant the primary framebuffer capability. */
cap_id_t kernel_capability_framebuffer_grant(process_id_t recipient);
