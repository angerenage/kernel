#ifndef TEST_KTHREAD_TEST_SUPPORT_H
#define TEST_KTHREAD_TEST_SUPPORT_H

#include <core/cpu.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <core/thread.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <setjmp.h>

#include "../mocks/hal/cpu_mock.h"

extern bool            kthread_test_cancel_hook_armed;
extern jmp_buf         kthread_test_cancel_jmp;
extern bool            kthread_test_sleep_cancel_hook_armed;
extern struct kthread* kthread_test_sleep_cancel_target;
extern bool            kthread_test_park_hook_armed;
extern struct kthread* kthread_test_park_target;
extern size_t          kthread_test_timeout_hook_runs;
extern size_t          kthread_test_park_hook_runs;

void init_bound_bootstrap_cpu(void);
void reset_test_state(void);
void kthread_test_init_scheduler(void);
void kthread_test_entry(void* arg);
void kthread_test_init_target(struct kthread* target, const char* name, uintptr_t stack_base, bool make_current);
void kthread_test_cancel_context_switch_hook(struct thread_context* current, const struct thread_context* next);
void kthread_test_sleep_cancel_context_switch_hook(struct thread_context* current, const struct thread_context* next);
void kthread_test_park_context_switch_hook(struct thread_context* current, const struct thread_context* next);
void kthread_test_timeout_context_switch_hook(struct thread_context* current, const struct thread_context* next);

#endif
