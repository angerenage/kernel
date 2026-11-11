#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <runtime/registry.h>

/* Replace the temporary bootfs root and retain its module directory at /boot. */
bool init_root_pivot(const struct registry_service_handle* vfs, cap_id_t new_root, uint32_t mount_flags);
