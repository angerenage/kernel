#ifndef TEST_CPU_TEST_SUPPORT_H
#define TEST_CPU_TEST_SUPPORT_H

#include <core/cpu.h>
#include <core/spinlock.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stdint.h>

extern const struct cpu_init_info cpu_test_valid_topology[2];

void cpu_test_init_bound_bootstrap(void);
void cpu_test_reset(void);

#endif
