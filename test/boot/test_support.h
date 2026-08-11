#ifndef TEST_BOOT_TEST_SUPPORT_H
#define TEST_BOOT_TEST_SUPPORT_H

#include <kernel/boot.h>
#include <limine.h>
#include <stdint.h>

void boot_test_reset(void);
void boot_test_configure_valid_base(void);
void boot_test_configure_module_count(uint64_t module_count);
void boot_test_configure_mp(struct LIMINE_MP(info) * *cpus, uint64_t cpu_count, uint64_t bsp_arch_id);

#endif
