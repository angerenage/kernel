#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>

/* Create the singleton framebuffer capability object. */
void kernel_capability_boot_resources_init(void);

/* Report whether a validated primary framebuffer is available. */
bool kernel_capability_framebuffer_available(void);

/* Grant the primary framebuffer capability. */
cap_id_t kernel_capability_framebuffer_grant(process_id_t recipient);
