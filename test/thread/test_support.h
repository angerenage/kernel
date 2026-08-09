#ifndef TEST_THREAD_TEST_SUPPORT_H
#define TEST_THREAD_TEST_SUPPORT_H

#include <core/cpu.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

void init_bound_bootstrap_cpu(void);
void reset_test_state(void);
void thread_test_entry(void* arg);
void thread_regression_reset(void);
void thread_regression_init(struct thread* thread, const char* name, uintptr_t stack_base, int32_t priority);

#endif
