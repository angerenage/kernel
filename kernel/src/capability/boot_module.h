#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stddef.h>
#include <stdint.h>

void     kernel_capability_boot_module_init(void);
cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t target);
