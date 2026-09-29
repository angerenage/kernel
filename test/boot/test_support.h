#ifndef TEST_BOOT_TEST_SUPPORT_H
#define TEST_BOOT_TEST_SUPPORT_H

#include <boot/protocol.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../boot/limine/limine.h"

void boot_test_reset(void);
void boot_test_configure_valid_base(void);
void boot_test_configure_memory_type(uint64_t type);
void boot_test_configure_module_count(uint64_t module_count);
void boot_test_set_dt_initialized(bool initialized);
void boot_test_configure_mp(struct limine_mp_info** cpus, uint64_t cpu_count, uint64_t bsp_arch_id);

#endif
