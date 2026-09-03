#ifndef TEST_SCHED_TEST_SUPPORT_H
#define TEST_SCHED_TEST_SUPPORT_H

#include <core/cpu.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/vm_space.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>

#include "../mocks/hal/cpu_mock.h"

uintptr_t                hal_paging_mock_active_root_phys(void);
void                     hal_paging_mock_reset_active(void);
struct hal_paging_space* hal_paging_mock_space(uintptr_t root_phys);

extern bool   sched_regression_reschedule_hook_armed;
extern size_t sched_regression_reap_count;

void init_bound_bootstrap_cpu(void);
void reset_test_state(void);
void sched_test_thread_entry(void* arg);
void sched_test_set_one_tick_timeslice(struct thread* thread);
void init_started_dual_cpu_topology(struct cpu** out_bsp, struct cpu** out_ap);

void sched_regression_reset(void);
void sched_regression_init_single_cpu(void);
void sched_regression_init_dual_cpu(struct cpu** out_bsp, struct cpu** out_ap);
void sched_regression_init_thread(struct thread* thread, const char* name, uintptr_t stack_base, int32_t priority,
                                  struct cpu* preferred_cpu, struct address_space* address_space);
void sched_regression_reschedule_hook(struct thread_context* current, const struct thread_context* next);
void sched_regression_reap_callback(struct thread* thread, void* ctx);

#endif
