#pragma once

#include <stdint.h>

uint64_t hal_cpu_boot_arch_id(void);
void*    hal_cpu_local_current(void);
void     hal_cpu_local_bind(void* ptr);
