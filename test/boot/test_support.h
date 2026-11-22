#ifndef TEST_BOOT_TEST_SUPPORT_H
#define TEST_BOOT_TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

#include <boot/protocol.h>

#include "../../boot/limine/limine.h"

extern volatile struct limine_tsc_frequency_request tsc_frequency_req;

void boot_test_reset(void);
void boot_test_configure_valid_base(void);
void boot_test_configure_memory_type(uint64_t type);
void boot_test_configure_module_count(uint64_t module_count);
void boot_test_set_dt_initialized(bool initialized);
void boot_test_configure_mp(struct limine_mp_info** cpus, uint64_t cpu_count, uint64_t bsp_arch_id);
limine_goto_address boot_test_mp_entry(void);
boot_cpu_entry_t boot_test_launch_entry(size_t index);
void* boot_test_launch_arg(size_t index);
size_t boot_test_launch_index(size_t index);
uintptr_t boot_test_launch_stack_top(size_t index);

#endif
